THINGINO_LIGHTGUN_BENCH_SITE_METHOD = local
THINGINO_LIGHTGUN_BENCH_SITE = $(THINGINO_LIGHTGUN_BENCH_PKGDIR)

THINGINO_LIGHTGUN_BENCH_DEPENDENCIES += ingenic-lib

ifeq ($(BR2_TOOLCHAIN_USES_MUSL),y)
	THINGINO_LIGHTGUN_BENCH_DEPENDENCIES += ingenic-musl
	THINGINO_LIGHTGUN_BENCH_SHIM = -lmuslshim
endif

ifeq ($(BR2_TOOLCHAIN_USES_UCLIBC),y)
	THINGINO_LIGHTGUN_BENCH_DEPENDENCIES += ingenic-uclibc
	THINGINO_LIGHTGUN_BENCH_SHIM = -luclibcshim
endif

# include/imp holds the T23 SDK 1.3.0 IMP headers (same copy prudynt-t vendors),
# matching the libimp that ingenic-lib installs for t23.
define THINGINO_LIGHTGUN_BENCH_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -O2 -Wall -Wextra -Wno-unused-parameter \
		-I$(@D)/include \
		-o $(@D)/lgcap $(@D)/lgcap.c \
		$(TARGET_LDFLAGS) \
		-L$(STAGING_DIR)/usr/lib \
		-L$(TARGET_DIR)/usr/lib \
		-limp -lalog \
		-Wl,--no-as-needed $(THINGINO_LIGHTGUN_BENCH_SHIM) -Wl,--as-needed \
		-lrt -lpthread -lm -ldl
endef

define THINGINO_LIGHTGUN_BENCH_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/lgcap $(TARGET_DIR)/usr/bin/lgcap
	$(INSTALL) -D -m 0755 $(THINGINO_LIGHTGUN_BENCH_PKGDIR)/lgbench.sh \
		$(TARGET_DIR)/usr/bin/lgbench
endef

$(eval $(generic-package))
