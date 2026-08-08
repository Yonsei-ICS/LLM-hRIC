#/bin/bash
# SPDX-License-Identifier: MIT

branch=$(git rev-parse --abbrev-ref HEAD)
commit=$(git rev-parse HEAD)
file=../../test_results.html
rm -f ${file}

cd ../../
python3 main.py \
  --mode=InitiateHtml \
  --repository=https://gitlab.eurecom.fr/oai/openairinterface5g.git \
  --branch=${branch} \
  --XMLTestFile=tests/test-runner/test.xml

python3 main.py \
  --mode=TesteNB \
  --repository=https://gitlab.eurecom.fr/oai/openairinterface5g.git \
  --branch=${branch} \
  --ranAllowMerge=true \
  --targetBranch=develop \
  --workspace=NONE \
  --XMLTestFile=tests/test-runner/test.xml

python3 main.py \
  --mode=FinalizeHtml \
  --finalStatus=true
cd -

if [ -f ${file} ]; then
  echo "results are in ${file}"
else
  echo "ERROR: no results file created"
fi
