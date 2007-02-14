TARGETS = index.html xlamesaver.html 

include ../common.rules

index.html: ASCIIDOC_PARAMS=-n
index.html: ../../xlamesaver/README ../credits.txt
	$(asciidoc)

xlamesaver.html: ../../xlamesaver/xlamesaver.py
	env PYTHONPATH=$$PYTHONPATH:../../xlamesaver \
		python mypydoc -w xlamesaver
	$(tidy)
