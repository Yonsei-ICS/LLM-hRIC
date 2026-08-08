PYTHON ?= /home/ics1/anaconda3/bin/python
# Point RESULTS at a PERSISTENT campaign directory, never /tmp: the pilot's raw
# databases were lost that way. 'make results' overwrites generated/, which is
# now the only surviving evidence for the manuscript.
RESULTS ?= $(HOME)/llm_hric_results/five-ue-traffic-v5
SPEC ?= ../../openair2/E2AP/flexric/examples/xApp/python3/llm_hric/experiments/five_ue_traffic_scenarios_v5.json

.PHONY: all results pdf test check clean

all: results check

results:
	$(PYTHON) scripts/build_results.py --results $(RESULTS) --spec $(SPEC) --output generated

pdf: results
	latexmk -pdf -interaction=nonstopmode -halt-on-error main.tex

test:
	$(PYTHON) -m unittest discover -s tests -p 'test_*.py'

check:
	$(PYTHON) scripts/check_sources.py

clean:
	latexmk -C 2>/dev/null || true
	rm -f main.bbl main.blg main.run.xml
