# Global
#
TARGETS = index.html credits.html
EXTRAS = pubkey.txt favicon.ico
include common.rules

# Local
#
DESC = $(shell for DIR in \
	`find .. -maxdepth 1 -mindepth 1 -type d -not -name 'site'` ; do \
		F=$$DIR/`basename $$DIR`.c ; \
		test -e $$F && echo $$F ; \
	done )
SUBDOCS = xlamesaver

index.html: index.txt top.txt others.txt
	$(asciidoc)

credits.html: ASCIIDOC_PARAMS+= -a "doctitle=License"
credits.html: credits.txt
	$(asciidoc)

.SECONDARY: index.txt

index.txt: $(DESC)
	(echo 'include::top.txt[]' ; \
	echo ; \
	for F in $(DESC); do \
		NAME=`basename $$F | sed 's/\.c$$//'` ; \
		echo $$NAME ; \
		echo $$NAME | sed 's/./-/g' ; \
		sed -n '1,/Legalese/p' $$F | sed '1d;$$d' ; \
	done;  \
	echo 'include::others.txt[]' ; \
	echo ; \
	for DIR in $(SUBDOCS) ; do \
		echo "- link:`basename $$DIR`/[`basename $$DIR`]" ; \
	done ; \
	echo ; \
	) > $@

all:
	$(recurse)

.PHONY: publish

clean:
	-find . -type f -name '*~' -exec rm \{\} ';'
	-rm -rf $(DEST) index.txt
	$(recurse)

prepare:
	$(recurse)

publish: prepare
	-rsync -rlptDvz ./tmp/* berlios:/home/groups/syfouxutils/htdocs
