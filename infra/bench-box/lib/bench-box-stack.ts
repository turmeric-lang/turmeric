import * as fs from 'fs';
import * as path from 'path';
import * as cdk from 'aws-cdk-lib';
import * as ec2 from 'aws-cdk-lib/aws-ec2';
import * as iam from 'aws-cdk-lib/aws-iam';
import { Construct } from 'constructs';

export interface BenchBoxStackProps extends cdk.StackProps {
  /** EC2 instance type, e.g. m7i.xlarge.  Must match `arch`. */
  readonly instanceType: string;
  /** CPU architecture; selects the Ubuntu AMI. */
  readonly arch: 'x86_64' | 'arm64';
  /** Root volume size in GiB (gp3).  Builds, ccache and ASan want room. */
  readonly diskGb: number;
  /** Branch, tag or commit of the Turmeric repo to clone and build at boot. */
  readonly gitRef: string;
  /** Clone URL of the Turmeric repo. */
  readonly repoUrl: string;
  /** The deadman timer: the box stops itself this many hours after boot. */
  readonly maxUptimeHours: number;
  /** Optional OpenSSH public key appended to ubuntu's authorized_keys. */
  readonly sshPublicKey?: string;
  /**
   * Optional name of a customer-managed policy applied as the permissions
   * boundary of every role in this stack.  The scoped CloudFormation
   * execution policy in iam/cfn-exec-policy.json refuses iam:CreateRole
   * without it, so the stack can never mint a role wider than the boundary.
   */
  readonly permissionsBoundaryName?: string;
}

/**
 * One throwaway EC2 instance for running Turmeric's suites and benchmarks
 * on a quiet, known machine.  `cdk deploy` is "spin up", `cdk destroy` is
 * "tear down"; nothing in this stack is meant to outlive a session.
 *
 * Cost shape, deliberately:
 *   - one public subnet, one AZ, no NAT gateway (the box reaches the internet
 *     through its own public IPv4; nothing reaches it -- the security group has
 *     no inbound rules and access is SSM Session Manager);
 *   - no Elastic IP, no load balancer, no VPC endpoints, no CloudWatch agent;
 *   - a deadman timer stops the instance after `maxUptimeHours`, so a
 *     forgotten box costs its EBS volume (cents a day), not its vCPUs.
 */
export class BenchBoxStack extends cdk.Stack {
  constructor(scope: Construct, id: string, props: BenchBoxStackProps) {
    super(scope, id, props);

    cdk.Tags.of(this).add('Project', 'turmeric');
    cdk.Tags.of(this).add('Purpose', 'bench-box');

    if (props.permissionsBoundaryName) {
      iam.PermissionsBoundary.of(this).apply(
        iam.ManagedPolicy.fromManagedPolicyName(this, 'InstanceBoundary', props.permissionsBoundaryName),
      );
    }

    const vpc = new ec2.Vpc(this, 'Vpc', {
      maxAzs: 1,
      natGateways: 0,
      subnetConfiguration: [{ name: 'public', subnetType: ec2.SubnetType.PUBLIC, cidrMask: 24 }],
      // The default-SG custom resource adds a Lambda and ~a minute to every
      // deploy; the instance never uses the default SG, so skip it.
      restrictDefaultSecurityGroup: false,
    });

    const sg = new ec2.SecurityGroup(this, 'Sg', {
      vpc,
      description: 'turmeric bench box: no inbound; SSM only',
      allowAllOutbound: true,
    });

    const role = new iam.Role(this, 'Role', {
      assumedBy: new iam.ServicePrincipal('ec2.amazonaws.com'),
      description: 'turmeric bench box instance role (SSM Session Manager only)',
    });

    const ubuntuArch = props.arch === 'arm64' ? 'arm64' : 'amd64';
    const machineImage = ec2.MachineImage.fromSsmParameter(
      `/aws/service/canonical/ubuntu/server/24.04/stable/current/${ubuntuArch}/hvm/ebs-gp3/ami-id`,
      { os: ec2.OperatingSystemType.LINUX },
    );

    const bootstrap = fs
      .readFileSync(path.join(__dirname, '..', 'bootstrap', 'user-data.sh'), 'utf8')
      .replace(/__REPO_URL__/g, props.repoUrl)
      .replace(/__GIT_REF__/g, props.gitRef)
      .replace(/__MAX_UPTIME_HOURS__/g, String(props.maxUptimeHours))
      .replace(/__SSH_PUBKEY__/g, props.sshPublicKey ?? '');
    const userData = ec2.UserData.custom(bootstrap);

    const instance = new ec2.Instance(this, 'Box', {
      vpc,
      vpcSubnets: { subnetType: ec2.SubnetType.PUBLIC },
      associatePublicIpAddress: true,
      instanceType: new ec2.InstanceType(props.instanceType),
      machineImage,
      securityGroup: sg,
      role,
      ssmSessionPermissions: true,
      requireImdsv2: true,
      userData,
      // A changed bootstrap script replaces the box: it is disposable, and
      // a half-bootstrapped instance is worse than a fresh one.
      userDataCausesReplacement: true,
      propagateTagsToVolumeOnCreation: true,
      blockDevices: [{
        deviceName: '/dev/sda1',
        volume: ec2.BlockDeviceVolume.ebs(props.diskGb, {
          volumeType: ec2.EbsDeviceVolumeType.GP3,
          encrypted: true,
          deleteOnTermination: true,
        }),
      }],
    });

    new cdk.CfnOutput(this, 'InstanceId', { value: instance.instanceId });
    new cdk.CfnOutput(this, 'PublicIp', { value: instance.instancePublicIp });
    new cdk.CfnOutput(this, 'InstanceType', { value: props.instanceType });
    new cdk.CfnOutput(this, 'Region', { value: this.region });
    new cdk.CfnOutput(this, 'SessionCommand', {
      value: `aws ssm start-session --region ${this.region} --target ${instance.instanceId}`,
    });
  }
}
