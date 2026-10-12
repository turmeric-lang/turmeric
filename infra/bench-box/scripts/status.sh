#!/usr/bin/env bash
# Is anything running?  Reads the account, not the outputs file, so it also
# finds a box this checkout did not deploy.
source "$(dirname "$0")/common.sh"
aws ec2 describe-instances \
  --filters "Name=tag:Purpose,Values=bench-box" "Name=instance-state-name,Values=pending,running,stopping,stopped" \
  --query 'Reservations[].Instances[].{Id:InstanceId,Type:InstanceType,State:State.Name,Up:LaunchTime,Ip:PublicIpAddress}' \
  --output table
