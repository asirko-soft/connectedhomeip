# Minimal makefile for Sphinx documentation
#

# You can set these variables from the command line, and also
# from the environment for the first two.
SPHINXOPTS    ?= -W -T --keep-going -c . -d _build/doctrees
SPHINXBUILD   ?= sphinx-build
SOURCEDIR     = _build/src
BUILDDIR      = _build
PYTHON_API_DIR= ${SOURCEDIR}/testing

# Put it first so that "make" without argument is like "make help".
help:
	@$(SPHINXBUILD) -M help "$(SOURCEDIR)" "$(BUILDDIR)" $(SPHINXOPTS) $(O)

.PHONY: help Makefile

# Catch-all target: route all unknown targets to Sphinx using the new
# "make mode" option.  $(O) is meant as a shortcut for $(SPHINXOPTS).
%: Makefile
	mkdir -p "$(SOURCEDIR)"
	mkdir -p "${PYTHON_API_DIR}"
	testing/GenerateChipDeviceCtrlDoc.sh ${PYTHON_API_DIR}
	@$(SPHINXBUILD) -M $@ "$(SOURCEDIR)" "$(BUILDDIR)" $(SPHINXOPTS) $(O)