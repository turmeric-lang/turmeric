#!/usr/bin/env bash
# One-time account setup for the bench box.  Run this ONCE, as a human with
# admin credentials, from your own machine.  It creates the three IAM
# policies in iam/, bootstraps CDK under its own qualifier (so it never
# touches an existing CDKToolkit stack), and optionally creates the
# least-privilege IAM user whose access key a Claude Code environment or a
# CI job gets.  docs/design/aws-bench-box-design.md "Credentials and
# permissions" explains each piece.
#
#   AWS_PROFILE=admin scripts/bootstrap-account.sh                 # policies + bootstrap
#   AWS_PROFILE=admin scripts/bootstrap-account.sh --create-user   # ...and the operator user + one key
#   AWS_PROFILE=admin scripts/bootstrap-account.sh --budget you@example.com [--budget-usd 25]
#
# Re-running is safe: policies get a new default version, bootstrap is
# idempotent, an existing user is left alone.
set -euo pipefail
cd "$(dirname "$0")/.."

CREATE_USER=0; BUDGET_EMAIL=""; BUDGET_USD=25
while [ $# -gt 0 ]; do
  case "$1" in
    --create-user) CREATE_USER=1 ;;
    --budget) BUDGET_EMAIL="$2"; shift ;;
    --budget-usd) BUDGET_USD="$2"; shift ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac; shift
done

ACCOUNT_ID="$(aws sts get-caller-identity --query Account --output text)"
REGION="${AWS_REGION:-${AWS_DEFAULT_REGION:-$(aws configure get region || true)}}"
[ -n "$REGION" ] || { echo "set AWS_REGION (the one region the box may live in)" >&2; exit 2; }
QUALIFIER=benchbox
USER_NAME=turmeric-bench-operator
echo "account $ACCOUNT_ID, region $REGION, qualifier $QUALIFIER"

render() { sed "s/\${ACCOUNT_ID}/$ACCOUNT_ID/g; s/\${REGION}/$REGION/g" "iam/$1.json"; }

# Create or version a customer-managed policy.  IAM keeps 5 versions; prune
# the oldest non-default one before adding so a re-run never fails on that.
upsert_policy() {
  local name="$1" file="$2" arn="arn:aws:iam::$ACCOUNT_ID:policy/$name" doc
  doc="$(render "$file")"
  if aws iam get-policy --policy-arn "$arn" >/dev/null 2>&1; then
    local old
    old="$(aws iam list-policy-versions --policy-arn "$arn" \
           --query 'Versions[?IsDefaultVersion==`false`] | sort_by(@, &CreateDate)[0].VersionId' --output text)"
    if [ "$(aws iam list-policy-versions --policy-arn "$arn" --query 'length(Versions)')" -ge 5 ] && [ "$old" != "None" ]; then
      aws iam delete-policy-version --policy-arn "$arn" --version-id "$old"
    fi
    aws iam create-policy-version --policy-arn "$arn" --policy-document "$doc" --set-as-default >/dev/null
    echo "updated  $arn"
  else
    aws iam create-policy --policy-name "$name" --policy-document "$doc" \
      --description "turmeric infra/bench-box: iam/$file.json" >/dev/null
    echo "created  $arn"
  fi
}

upsert_policy TurmericBenchBoxInstanceBoundary instance-boundary
upsert_policy TurmericBenchBoxCfnExec          cfn-exec-policy
upsert_policy TurmericBenchBoxOperator         operator-policy

# CDK bootstrap under its own qualifier and toolkit stack: roles named
# cdk-benchbox-*, staging bucket cdk-benchbox-assets-*, and a CloudFormation
# execution role carrying OUR policy instead of AdministratorAccess.
[ -d node_modules ] || npm ci --no-audit --no-fund
npx cdk bootstrap "aws://$ACCOUNT_ID/$REGION" \
  --qualifier "$QUALIFIER" \
  --toolkit-stack-name "CDKToolkit-$QUALIFIER" \
  --cloudformation-execution-policies "arn:aws:iam::$ACCOUNT_ID:policy/TurmericBenchBoxCfnExec" \
  --tags Project=turmeric --tags Purpose=bench-box

if [ "$CREATE_USER" = 1 ]; then
  if aws iam get-user --user-name "$USER_NAME" >/dev/null 2>&1; then
    echo "user $USER_NAME exists; not touching it or its keys"
  else
    aws iam create-user --user-name "$USER_NAME" --tags Key=Project,Value=turmeric Key=Purpose,Value=bench-box >/dev/null
    aws iam attach-user-policy --user-name "$USER_NAME" \
      --policy-arn "arn:aws:iam::$ACCOUNT_ID:policy/TurmericBenchBoxOperator"
    echo "created user $USER_NAME with TurmericBenchBoxOperator attached"
    echo
    echo "Creating its one access key.  The secret is shown ONCE, here, and nowhere"
    echo "else -- store it straight into the consumer (a Claude Code environment's"
    echo "secrets, a CI secret store), never in a file in this repo or in a chat."
    aws iam create-access-key --user-name "$USER_NAME" \
      --query 'AccessKey.{AWS_ACCESS_KEY_ID:AccessKeyId,AWS_SECRET_ACCESS_KEY:SecretAccessKey}' --output table
    echo "also set: AWS_DEFAULT_REGION=$REGION"
  fi
fi

if [ -n "$BUDGET_EMAIL" ]; then
  # A monthly cost budget filtered to the bench-box tag, alerting at 50/80/100%
  # of actual spend.  Needs cost-allocation tag activation for Purpose in the
  # Billing console once (takes ~24h to start filtering); until then the
  # budget reads 0.  Budgets live in us-east-1 regardless of REGION.
  cat > /tmp/bench-budget.json <<JSON
{"BudgetName":"turmeric-bench-box","BudgetType":"COST","TimeUnit":"MONTHLY",
 "BudgetLimit":{"Amount":"$BUDGET_USD","Unit":"USD"},
 "CostFilters":{"TagKeyValue":["user:Purpose\$bench-box"]}}
JSON
  notifs=()
  for pct in 50 80 100; do notifs+=("{\"Notification\":{\"NotificationType\":\"ACTUAL\",\"ComparisonOperator\":\"GREATER_THAN\",\"Threshold\":$pct,\"ThresholdType\":\"PERCENTAGE\"},\"Subscribers\":[{\"SubscriptionType\":\"EMAIL\",\"Address\":\"$BUDGET_EMAIL\"}]}"); done
  if aws budgets describe-budget --account-id "$ACCOUNT_ID" --budget-name turmeric-bench-box >/dev/null 2>&1; then
    aws budgets update-budget --account-id "$ACCOUNT_ID" --new-budget file:///tmp/bench-budget.json
    echo "updated budget turmeric-bench-box (\$$BUDGET_USD/month)"
  else
    aws budgets create-budget --account-id "$ACCOUNT_ID" --budget file:///tmp/bench-budget.json \
      --notifications-with-subscribers "[$(IFS=,; echo "${notifs[*]}")]"
    echo "created budget turmeric-bench-box (\$$BUDGET_USD/month, alerts to $BUDGET_EMAIL)"
  fi
  rm -f /tmp/bench-budget.json
fi

echo
echo "done.  Operators (humans via SSO, or the $USER_NAME key) can now run scripts/up.sh."
