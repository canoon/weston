protocol_sources =				\
	desktop-shell.xml			\
	screenshooter.xml			\
	tablet-shell.xml			\
	xserver.xml				\
	text.xml				\
	input-method.xml			\
	workspaces.xml				\
	subsurface.xml				\
	text-cursor-position.xml		\
	wayland-test.xml

if HAVE_XMLLINT
.PHONY: validate

.%.xml.valid: %.xml
	$(AM_V_GEN)$(XMLLINT) --noout --dtdvalid $(dtddir)/wayland.dtd $^ > $@

validate: $(protocol_sources:%.xml=.%.xml.valid)

all-local: validate

CLEANFILES= $(protocol_sources:%.xml=.%.xml.valid)
EXTRA_DIST=$(protocol_sources)

endif
