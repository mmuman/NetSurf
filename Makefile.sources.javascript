#
# NetSurf javascript source file inclusion
#
# Included by Makefile.sources 
#

# ----------------------------------------------------------------------------
# JSAPI binding
# ----------------------------------------------------------------------------

S_JSAPI_BINDING:=

JSAPI_BINDING_htmldocument := javascript/jsapi/bindings/htmldocument.bnd

# 1: input file
# 2: output file
# 3: binding name
define convert_jsapi_binding

S_JSAPI_BINDING += $(2)

$(2): $(1)
	$(Q)nsgenbind -I javascript/jsapi/WebIDL/ -o $(2) $(1)

endef

# Javascript sources
ifeq ($(NETSURF_USE_JS),YES)

S_JSAPI = window.c navigator.c console.c htmlelement.c
#htmldocument.c

S_JAVASCRIPT += content.c jsapi.c $(addprefix jsapi/,$(S_JSAPI))

$(eval $(foreach V,$(filter JSAPI_BINDING_%,$(.VARIABLES)),$(call convert_jsapi_binding,$($(V)),$(OBJROOT)/$(patsubst JSAPI_BINDING_%,%,$(V)).c,$(patsubst JSAPI_BINDING_%,%,$(V))_jsapi)))


else
S_JAVASCRIPT += none.c
endif
