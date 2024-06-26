PORTNAME=	moonlight-embedded
DISTVERSION=	2.7.0
CATEGORIES=	games
MASTER_SITES=	https://github.com/moonlight-stream/moonlight-embedded/releases/download/v${DISTVERSION}/

MAINTAINER=	lisp_25689@163.com
COMMENT=	Gamestream client
WWW=		https://github.com/moonlight-stream/moonlight-embedded

LICENSE=	GPLv3+
LICENSE_FILE=	${WRKSRC}/LICENSE

BUILD_DEPENDS=	${LOCALBASE}/include/linux/input.h:devel/evdev-proto \
		${LOCALBASE}/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml:graphics/wayland-protocols
LIB_DEPENDS=	libavahi-client.so:net/avahi-app \
		libavcodec.so:multimedia/ffmpeg \
		libcurl.so:ftp/curl \
		libepoll-shim.so:devel/libepoll-shim \
		libevdev.so:devel/libevdev \
		libexpat.so:textproc/expat2 \
		libopus.so:audio/opus \
		libudev.so:devel/libudev-devd \
		libuuid.so:misc/e2fsprogs-libuuid \
		libvdpau.so:multimedia/libvdpau \
		libwayland-client.so:graphics/wayland \
		libva.so:multimedia/libva

USES=		cmake gl localbase:ldflags perl5 pkgconfig sdl ssl tar:xz xorg
USE_LDCONFIG=	yes
USE_GL=		egl glesv2
USE_PERL5=	build
USE_SDL=	sdl2
USE_XORG=	x11

NO_WRKSUBDIR=	yes
CFLAGS+=	-DHAS_SOCKLEN_T=1 -I${LOCALBASE}/include/libepoll-shim/
LDFLAGS+=	-lepoll-shim
CMAKE_OFF=	ENABLE_CEC ENABLE_PULSE
CMAKE_ARGS=	-DCMAKE_INSTALL_SYSCONFDIR=${PREFIX}/etc/moonlight

PLIST_FILES=	bin/moonlight \
		"@sample etc/moonlight/moonlight.conf.sample" \
		share/man/man1/moonlight.1.gz \
		share/moonlight/gamecontrollerdb.txt

post-patch:
	@${REINPLACE_CMD} -e 's@/usr/local@${PREFIX}@' \
		${WRKSRC}/docs/README.pod
	@${REINPLACE_CMD} -e 's@/usr/local@${PREFIX}@' \
		${WRKSRC}/src/config.c

post-install:
	@${MV} ${STAGEDIR}${PREFIX}/etc/moonlight/moonlight.conf \
		${STAGEDIR}${PREFIX}/etc/moonlight/moonlight.conf.sample

.include <bsd.port.mk>
