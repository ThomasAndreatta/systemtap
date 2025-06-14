# Makefile.am --- automake input file for systemtap
## process this file with automake to produce Makefile.in

# we don't maintain a ChangeLog, which makes us non-GNU -> foreign
AUTOMAKE_OPTIONS = no-dist foreign subdir-objects

pkglibexecdir = ${libexecdir}/${PACKAGE}
oldincludedir = ${includedir}/sys

AM_CPPFLAGS = -DBINDIR='"$(bindir)"' \
	      -DSYSCONFDIR='"$(sysconfdir)"' \
	      -DPKGDATADIR='"$(pkgdatadir)"' \
	      -DPKGLIBDIR='"$(pkglibexecdir)"' \
	      -DLOCALEDIR='"$(localedir)"' \
              -DDOCDIR='"$(docdir)"' \
              -DPYEXECDIR='"$(pyexecdir)"' \
              -DPY3EXECDIR='"$(py3execdir)"' \
	      -I$(srcdir)/includes -I$(builddir)/includes/sys

AM_CFLAGS = -D_GNU_SOURCE -fexceptions -Wall -Wextra -Wunused -Wformat=2 -W
AM_CXXFLAGS = -Wall -Wextra
if Werror
AM_CFLAGS += -Werror
AM_CXXFLAGS += -Werror
endif

bin_SCRIPTS = stap-report stap-profile-annotate
pkglibexec_SCRIPTS = stap-env
oldinclude_HEADERS = includes/sys/sdt.h includes/sys/sdt-config.h

bin_PROGRAMS =

if BUILD_TRANSLATOR
bin_PROGRAMS += stap
bin_SCRIPTS += dtrace

pkglibexec_PROGRAMS =

if HAVE_NSS
pkglibexec_PROGRAMS += stap-sign-module stap-authorize-cert

if BUILD_SERVER
pkglibexec_PROGRAMS += stap-serverd stap-gen-cert
bin_SCRIPTS += stap-server
pkglibexec_SCRIPTS += stap-start-server stap-stop-server
endif
endif

stap_SOURCES = main.cxx session.cxx \
	parse.cxx staptree.cxx elaborate.cxx translate.cxx \
	tapsets.cxx buildrun.cxx loc2stap.cxx hash.cxx mdfour.c \
	cache.cxx util.cxx coveragedb.cxx dwarf_wrappers.cxx \
	tapset-been.cxx tapset-procfs.cxx tapset-timers.cxx tapset-netfilter.cxx \
	tapset-perfmon.cxx tapset-mark.cxx \
	tapset-utrace.cxx task_finder.cxx dwflpp.cxx rpm_finder.cxx \
	setupdwfl.cxx remote.cxx privilege.cxx cmdline.cxx \
	tapset-dynprobe.cxx tapset-method.cxx translator-output.cxx \
        stapregex.cxx stapregex-tree.cxx stapregex-parse.cxx \
	stapregex-dfa.cxx stringtable.cxx tapset-python.cxx \
	tapset-debuginfod.cxx analysis.cxx
noinst_HEADERS = sdt_types.h
stap_LDADD = @stap_LIBS@ @sqlite3_LIBS@ @LIBINTL@ -lpthread @debuginfod_LDFLAGS@ @debuginfod_LIBS@ @DYNINST_LDFLAGS@ @DYNINST_LIBS@
stap_DEPENDENCIES =

if HAVE_LANGUAGE_SERVER_SUPPORT
stap_SOURCES += language-server/stap-language-server.cxx \
	language-server/stap-code-completion.cxx language-server/jsonrpc.cxx
endif

if HAVE_LIBREADLINE
stap_SOURCES += interactive.cxx
stap_LDADD += @READLINE_LIBS@
endif

if HAVE_JSON_C
stap_LDADD += $(jsonc_LIBS)
endif

if HAVE_BPF_DECLS
stap_SOURCES += bpf-base.cxx bpf-bitset.cxx bpf-translate.cxx bpf-opt.cxx
endif

if BUILD_VIRT
bin_PROGRAMS += stapvirt
stapvirt_SOURCES = stapvirt.c
stapvirt_CFLAGS = $(AM_CFLAGS)
stapvirt_CFLAGS += $(libvirt_CFLAGS) $(libxml2_CFLAGS)
stapvirt_LDFLAGS = $(AM_LDFLAGS)
stapvirt_LDADD = $(libvirt_LIBS) $(libxml2_LIBS)
endif

endif

all: macros.systemtap

macros.systemtap: $(srcdir)/macros.systemtap.in
	sed -e "s#@pkgdatadir@#$(pkgdatadir)#g" < $< > $@

install: all
	$(MAKE) $(AM_MAKEFLAGS) install-recursive

BUILT_SOURCES =
CLEANFILES =

# Arrange for git_version.h to be regenerated at every "make".
# Code fragment is based upon RadeonHD.am.

# The stamp file which is never created ensures that git_version.h is updated
# before every build. Having git_version.h in foo_SOURCES ensures a recompile
# of foo-bar.c if it is newer than the foo-bar.o file. Using noinst_foo_SOURCES
# instead of foo_SOURCES prevents shipping git_version.h in dist tarballs,
# which may cause false GIT_FOO readings.
BUILT_SOURCES += git_version.stamp
CLEANFILES += git_version.h
GIT_VERSION_CMD = $(SHELL) $(top_srcdir)/git_version.sh
git_version.stamp:
	@if test -f "$(srcdir)/git_version.h"; then \
		if test -f "git_version.h"; then :; \
		else \
			cp "$(srcdir)/git_version.h" "git_version.h"; \
		fi; \
	fi
	$(GIT_VERSION_CMD) -k -s $(top_srcdir) -o git_version.h
	@if test -s "$(srcdir)/git_version.h"; then \
		if cmp "$(srcdir)/git_version.h" "git_version.h"; then :; \
		else \
			echo "Error: $(srcdir)/git_version.h and git_version.h differ."; \
			echo "       You probably want to remove the former."; \
			exit 1; \
		fi; \
	fi

git_version.h:
	$(srcdir)/git_version.sh -k --srcdir $(srcdir) -o git_version.h

cscope:
	cd $(srcdir) && \
	(echo -q ; git ls-files '*.cxx' '*.c' '*.h' | grep -v '^testsuite' ) > cscope.files && \
	cscope -b -q @DYNINST_CXXFLAGS@

PHONIES =
if BUILD_TRANSLATOR
stap_CFLAGS = $(AM_CFLAGS) @PIECFLAGS@
stap_CXXFLAGS = $(AM_CXXFLAGS) @PIECXXFLAGS@ @ALIGNEDNEW@ @debuginfod_CFLAGS@
stap_CPPFLAGS = $(AM_CPPFLAGS) -DSTAP_SDT_V2
stap_LDFLAGS = $(AM_LDFLAGS) @PIELDFLAGS@ @BOOST_SYSTEM_LIB@

if HAVE_DYNINST
stap_CXXFLAGS += $(DYNINST_CXXFLAGS)
endif

if HAVE_AVAHI
stap_CXXFLAGS += $(avahi_CFLAGS)
stap_CPPFLAGS += $(avahi_CFLAGS)
stap_LDADD += $(avahi_LIBS)
endif

if NEED_BASE_CLIENT_CODE
stap_SOURCES += csclient.cxx
endif

if HAVE_NSS
stap_SOURCES += nsscommon.cxx client-nss.cxx cscommon.cxx nss-server-info.cxx
stap_CFLAGS += $(nss_CFLAGS) -DSTAP
stap_CXXFLAGS += $(nss_CFLAGS)
stap_CPPFLAGS += $(nss_CFLAGS)
stap_LDADD += $(nss_LIBS)

install-exec-local: install-scripts

PHONIES += install-scripts
# scripts must be installed before this rule is run
install-scripts: install-binSCRIPTS install-pkglibexecSCRIPTS
	for f in $(bin_SCRIPTS); do \
	   sed -i -e "s,\$${PKGLIBEXECDIR},$(pkglibexecdir)/," $(DESTDIR)$(bindir)/$$f; \
	done
	for f in $(pkglibexec_SCRIPTS); do \
	   sed -i -e "/INSTALL-HOOK/d;s,sysconfdir=.*,sysconfdir=$(sysconfdir),;s,pkglibexecdir=.*,pkglibexecdir=$(pkglibexecdir)/,;s,localstatedir=.*,localstatedir=$(localstatedir),;s,\$${PKGLIBEXECDIR},$(pkglibexecdir)/," $(DESTDIR)$(pkglibexecdir)/$$f; \
	done
endif

if HAVE_HTTP_SUPPORT
stap_SOURCES += client-http.cxx
stap_LDADD += $(libcurl_LIBS) $(jsonc_LIBS) $(openssl_LIBS)
endif

endif

CLEANFILES +=  $(pkglibexec_PROGRAMS)

if BUILD_TRANSLATOR
if HAVE_NSS
stap_sign_module_SOURCES = stap-sign-module.cxx nsscommon.cxx util.cxx
stap_sign_module_CPPFLAGS = $(AM_CPPFLAGS)
stap_sign_module_CXXFLAGS = $(AM_CXXFLAGS) @PIECXXFLAGS@ $(nss_CFLAGS) $(debuginfod_CFLAGS)
stap_sign_module_CFLAGS = $(AM_CFLAGS) @PIECFLAGS@ $(nss_CFLAGS) $(debuginfod_CFLAGS)
stap_sign_module_LDFLAGS = $(AM_LDFLAGS) @PIELDFLAGS@ $(debuginfod_LDFLAGS)
stap_sign_module_LDADD = $(nss_LIBS) $(debuginfod_LIBS)
if HAVE_HTTP_SUPPORT
stap_sign_module_LDADD += $(openssl_LIBS)
endif

stap_authorize_cert_SOURCES = stap-authorize-cert.cxx nsscommon.cxx util.cxx
stap_authorize_cert_CPPFLAGS = $(AM_CPPFLAGS)
stap_authorize_cert_CXXFLAGS = $(AM_CXXFLAGS) @PIECXXFLAGS@ $(nss_CFLAGS) $(debuginfod_CFLAGS)
stap_authorize_cert_CFLAGS = $(AM_CFLAGS) @PIECFLAGS@ $(nss_CFLAGS) $(debuginfod_CFLAGS)
stap_authorize_cert_LDFLAGS = $(AM_LDFLAGS) @PIELDFLAGS@ $(debuginfod_LDFLAGS)
stap_authorize_cert_LDADD = $(nss_LIBS) $(debuginfod_LIBS)
if HAVE_HTTP_SUPPORT
stap_authorize_cert_LDADD += $(openssl_LIBS)
endif

if BUILD_SERVER
stap_serverd_SOURCES = stap-serverd.cxx cscommon.cxx util.cxx privilege.cxx nsscommon.cxx cmdline.cxx
stap_serverd_CXXFLAGS = $(AM_CXXFLAGS) @PIECXXFLAGS@ $(nss_CFLAGS) $(debuginfod_CFLAGS)
stap_serverd_CFLAGS = $(AM_CFLAGS) @PIECFLAGS@ $(nss_CFLAGS) $(debuginfod_CFLAGS)
stap_serverd_LDFLAGS = $(AM_LDFLAGS) @PIELDFLAGS@ $(debuginfod_LDFLAGS)
stap_serverd_LDADD = $(nss_LIBS) -lpthread $(debuginfod_LIBS)
if HAVE_AVAHI
stap_serverd_CFLAGS += $(avahi_CFLAGS)
stap_serverd_CXXFLAGS += $(avahi_CFLAGS)
stap_serverd_LDADD += $(avahi_LIBS)
endif
if HAVE_HTTP_SUPPORT
stap_serverd_LDADD += $(openssl_LIBS)
endif


stap_gen_cert_SOURCES = stap-gen-cert.cxx util.cxx nsscommon.cxx
stap_gen_cert_CXXFLAGS = $(AM_CXXFLAGS) @PIECXXFLAGS@ $(nss_CFLAGS) $(debuginfod_CFLAGS)
stap_gen_cert_CFLAGS = $(AM_CFLAGS) @PIECFLAGS@ $(nss_CFLAGS) $(debuginfod_CFLAGS)
stap_gen_cert_LDFLAGS = $(AM_LDFLAGS) @PIELDFLAGS@ $(debuginfod_LDFLAGS)
stap_gen_cert_LDADD = $(nss_LIBS) $(debuginfod_LIBS)
if HAVE_HTTP_SUPPORT
stap_gen_cert_LDADD += $(openssl_LIBS)
endif
endif
endif
endif

# crash(8) extension
if BUILD_CRASHMOD
STAPLOG=staplog.so

$(STAPLOG): staplog.c
	$(CC) $(staplog_CPPFLAGS) $(AM_CFLAGS) $(CFLAGS) -shared -rdynamic \
		$(AM_LDFLAGS) $(LDFLAGS) -fPIC -o $@ $<
all-local: $(STAPLOG) example_index
install-exec-local: $(STAPLOG)
	$(MKDIR_P) $(DESTDIR)$(pkglibdir)
	$(INSTALL) $(STAPLOG) $(DESTDIR)$(pkglibdir)
else
all-local: example_index
endif

# Get extra libs as needed
LDADD =

EXAMPLE_SOURCE_DIR = $(srcdir)/testsuite/systemtap.examples
EXAMPLE_META_FILES = $(EXAMPLE_SOURCE_DIR)/*/*.meta

example_index: $(EXAMPLE_SOURCE_DIR)/index.html

$(EXAMPLE_SOURCE_DIR)/index.html: $(EXAMPLE_META_FILES) $(EXAMPLE_INDEX_GEN)
	@echo "The script example index is outdated, run"
	@echo "    cd $(EXAMPLE_SOURCE_DIR) && \\ "
	@echo "    perl examples-index-gen.pl"
	@echo "to regenerate it."

install-data-local:
	(cd $(srcdir)/runtime; for f in *.[ch]; do $(INSTALL_DATA) -D $$f $(DESTDIR)$(pkgdatadir)/runtime/$$f; done)
if HAVE_DYNINST
	(cd $(srcdir)/runtime/dyninst; find . \( -name '*.[ch]' \) -print \
		| while read f; do $(INSTALL_DATA) -D $$f $(DESTDIR)$(pkgdatadir)/runtime/dyninst/$$f; done)
endif
	(cd $(srcdir)/runtime/unwind; find . \( -name '*.[ch]' \) -print \
		| while read f; do $(INSTALL_DATA) -D $$f $(DESTDIR)$(pkgdatadir)/runtime/unwind/$$f; done)
	(cd $(srcdir)/runtime/transport; for f in *.[ch]; \
		do $(INSTALL_DATA) -D $$f $(DESTDIR)$(pkgdatadir)/runtime/transport/$$f; done)
	(cd $(srcdir)/runtime/softfloat; for f in *.[ch]; \
		do $(INSTALL_DATA) -D $$f $(DESTDIR)$(pkgdatadir)/runtime/softfloat/$$f; done)
	(cd $(srcdir)/runtime/linux; for f in *.[ch]; \
		do $(INSTALL_DATA) -D $$f $(DESTDIR)$(pkgdatadir)/runtime/linux/$$f; done)
	(cd $(srcdir)/tapset; find . \( -name '*.stp' -o -name '*.stpm' -o -name README \) -print \
		| while read f; do $(INSTALL_DATA) -D $$f $(DESTDIR)$(pkgdatadir)/tapset/$$f; done)
	(cd $(srcdir)/testsuite/systemtap.examples; find . -type f -print \
		| grep -E -v 'check.exp|ChangeLog|examples-index-gen.pl|html/.*\.tmpl|metadatabase.dbq$$' \
		| while read f; do if test -x $$f; then \
			i_cmd="$(INSTALL_PROGRAM)"; else \
			i_cmd="$(INSTALL_DATA)"; fi; \
			$$i_cmd -D $$f $(DESTDIR)$(pkgdatadir)/examples/$$f; done)
	test -e $(DESTDIR)$(sysconfdir)/systemtap || mkdir -p $(DESTDIR)$(sysconfdir)/systemtap
if BUILD_SERVER
	test -e $(DESTDIR)$(localstatedir)/run/stap-server || mkdir -p $(DESTDIR)$(localstatedir)/run/stap-server
	test -e $(DESTDIR)$(localstatedir)/log || mkdir -p $(DESTDIR)$(localstatedir)/log
endif


TEST_COV_DIR = coverage

gcov:
	find . -name '*.gc*' | xargs rm -f
	$(MAKE) clean CXXFLAGS="-g -fprofile-arcs -ftest-coverage" CFLAGS="-g -fprofile-arcs -ftest-coverage" all install
	-sudo $(MAKE) installcheck $${RUNTESTFLAGS+RUNTESTFLAGS=$${RUNTESTFLAGS}}
	for dir in $(SUBDIRS); do \
		(cd $$dir; gcov *.gcno > SUMMARY.gcov 2>&1 || true); \
	done
	find . -name '*.gcov'

clean-local:
	rm -rf ${PACKAGE_TARNAME}-*.tar.gz
	rm -rf ${PACKAGE_TARNAME}-${PACKAGE_VERSION}
	rm -rf $(TEST_COV_DIR)
	rm -rf stap.info
	rm -rf staplog.so

distclean-local:
	rm -f doc/beginners/en-US doc/beginners/build/en-US/testsuite

uninstall-local:
	rm -rf $(DESTDIR)$(pkgdatadir)
	rm -rf $(DESTDIR)$(libdir)/$(PACKAGE)
	-rm -rf $(DESTDIR)$(localstatedir)/cache/$(PACKAGE)
	@list='$(EXAMPLE_SRC)'; for f in $$list; do p=`basename $$f`; \
	  echo " rm -f '$(EXAMPLE_INSTALL_DIR)/$$p'"; \
	  rm -f "$(EXAMPLE_INSTALL_DIR)/$$p"; \
	done
	@list='$(DEMO_SRC)'; for f in $$list; do p=`basename $$f`; \
	  echo " rm -f '$(DEMO_INSTALL_DIR)/$$p'"; \
	  rm -f "$(DEMO_INSTALL_DIR)/$$p"; \
	done
	@list='$(SAMPLE_SRC)'; for f in $$list; do p=`basename $$f`; \
	  echo " rm -f '$(SAMPLE_INSTALL_DIR)/$$p'"; \
	  rm -f "$(SAMPLE_INSTALL_DIR)/$$p"; \
	done
	rm -rf $(DESTDIR)$(sysconfdir)/systemtap
	rm -rf $(DESTDIR)$(docdir)/examples
if BUILD_SERVER
	rm -rf $(DESTDIR)$(localstatedir)/run/stap-server
	rm -f $(DESTDIR)$(localstatedir)/log/stap-server/log
endif

SUBDIRS = . java python stapdyn staprun stapbpf doc man po httpd stap-exporter interactive-notebook
# NB: the gcov target above uses this to enumarate linked binaries' build directories
DIST_SUBDIRS = testsuite $(SUBDIRS)
EXTRA_DIST = m4/ChangeLog

check-local:
	SRCDIR=`cd $(srcdir); pwd`; \
        PWD=`pwd`; \
	$(MAKE) -C testsuite check SYSTEMTAP_RUNTIME=$$SRCDIR/runtime SYSTEMTAP_TAPSET=$$SRCDIR/tapset LD_LIBRARY_PATH=$$LD_LIBRARY_PATH$${LD_LIBRARY_PATH:+:}$$PWD/lib-elfutils:$$PWD/lib-elfutils/systemtap SYSTEMTAP_PATH=$$PWD SYSTEMTAP_INCLUDES=$$PWD/includes RUNTESTFLAGS="$(RUNTESTFLAGS)" PKGLIBDIR="$(pkglibexecdir)";

installcheck:
	if test \! -e $(DESTDIR)$(bindir)/stap; then \
	  echo $(DESTDIR)$(bindir)/stap doesn\'t exist, run make install; \
	  exit -1; \
	fi; \
	if test $(builddir)/stap -nt $(DESTDIR)$(bindir)/stap; then \
	  echo "$(DESTDIR)$(bindir)/stap is not recent, run make install"; \
	  exit -1; \
	fi;
	$(MAKE) -C testsuite installcheck RUNTESTFLAGS="$(RUNTESTFLAGS)"

installcheck-parallel:
	if test \! -e $(DESTDIR)$(bindir)/stap; then \
	  echo $(DESTDIR)$(bindir)/stap doesn\'t exist, run make install; \
	  exit -1; \
	fi; \
	if test $(builddir)/stap -nt $(DESTDIR)$(bindir)/stap; then \
	  echo "$(DESTDIR)$(bindir)/stap is not recent, run make install"; \
	  exit -1; \
	fi;
	$(MAKE) -C testsuite installcheck-parallel RUNTESTFLAGS="$(RUNTESTFLAGS)"

list-unfinished:
	if test \! -e $(DESTDIR)$(bindir)/stap; then \
	  echo $(DESTDIR)$(bindir)/stap doesn\'t exist, run make install; \
	  exit -1; \
	fi; \
	if test $(builddir)/stap -nt $(DESTDIR)$(bindir)/stap; then \
	  echo "$(DESTDIR)$(bindir)/stap is not recent, run make install"; \
	  exit -1; \
	fi;
	$(MAKE) -C testsuite list-unfinished RUNTESTFLAGS="$(RUNTESTFLAGS)"

PHONIES += runcheck
# All the variables are overridden by run-stap, but SYSTEMTAP_RUNTIME
# is used by the testsuite scripts themselves.
runcheck: testsuite/stap testsuite/dtrace
	$(MAKE) -C testsuite installcheck RUNTESTFLAGS="$(RUNTESTFLAGS)" \
		SYSTEMTAP_PATH="`cd testsuite; pwd`" \
		SYSTEMTAP_RUNTIME="`cd $(srcdir)/runtime; pwd`"
testsuite/stap: run-stap
	(echo '#!/bin/sh'; echo "exec `pwd`/run-stap" '$${1+"$$@"}') > $@.new
	chmod 555 $@.new
	mv -f $@.new $@
testsuite/dtrace: dtrace
	(echo '#!/bin/sh'; echo "exec `pwd`/dtrace" '$${1+"$$@"}') > $@.new
	chmod 555 $@.new
	mv -f $@.new $@

PHONIES += update-po
update-po:
	(cd $(srcdir); ls -1d *.c *.cxx *.h staprun/*.c staprun/*.h) \
	| grep -v loc2c-test.c \
	| grep -v config.h \
	| grep -v git_version.h \
	| grep -v staprun/config.h \
	| sort > $(srcdir)/po/POTFILES.in
	$(MAKE) -C po update-po
	@echo
	@echo if systemtam.pot is shown as modified ...
	@echo
	(cd $(srcdir)/po; git status *.pot)
	@echo
	@echo ... then check in with zanata ...
	@echo
	@echo % cd $(srcdir)
	@echo % zanata-cli -B push -s po -t po
	@echo % zanata-cli -B pull -s po -t po
	@echo
	@echo if the .po files are shown as modified ...
	@echo
	(cd $(srcdir)/po; git status *.po)
	@echo
	@echo ... regenerate just once ...
	@echo
	@echo % $(MAKE) -C `pwd` update-po


# Any extra flags, such as:
#     --define "with_docs 0"
#     --define "elfutils_version 0.135"
RPMBUILDFLAGS=

PHONIES += dist-gzip rpm srpm

PHONIES += uprobes install-uprobes
uprobes:
	$(MAKE) -C $(srcdir)/runtime/uprobes clean default
install-uprobes:
	$(MAKE) -C $(DESTDIR)$(pkgdatadir)/runtime/uprobes clean default

.PHONY: $(PHONIES)

dist-gzip:
	cd $(srcdir); git status | grep -E 'working.(tree|directory).clean' || (echo "You should commit your changes before 'make rpm'.")
	(cd $(srcdir); git archive --prefix=systemtap-$(VERSION)/ --format=tar HEAD) | gzip > systemtap-$(VERSION).tar.gz

rpm: dist-gzip
	if [ `id -u` = 0 ]; then echo NOTE: do not run this as root; exit 1; fi
	if [ ! -w `rpm --eval %_topdir` ]; then \
	  echo WARNING: you do not have access to `rpm --eval %_topdir`; \
	  echo WARNING: you may receive a permission denied error; \
	  echo WARNING: consider adding \'%_topdir $(HOME)/rpmbuild\' to $(HOME)/.rpmmacros; \
	fi
	mkdir -p `rpm --eval %_specdir`
	mkdir -p `rpm --eval %_srcrpmdir`
	mkdir -p `rpm --eval %_rpmdir`
	mkdir -p `rpm --eval %_builddir`
	mkdir -p `rpm --eval %_buildroot`
	rpmbuild --define "_sourcedir $(PWD)/" --define "release_override .`date +%Y%m%d%H%M`" -ta systemtap-$(VERSION).tar.gz $(RPMBUILDFLAGS)

srpm: dist-gzip
	mkdir -p `rpm --eval %_srcrpmdir`
	rpmbuild --define "_sourcedir $(PWD)/" -ts systemtap-$(VERSION).tar.gz $(RPMBUILDFLAGS)

ACLOCAL_AMFLAGS = -I m4
