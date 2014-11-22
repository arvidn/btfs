TARGETS = btfs

pdf: $(TARGETS:=.pdf)

all: pdf

%.pdf:%.rst
	rst2pdf $? -o $@ --stylesheets stylesheet

