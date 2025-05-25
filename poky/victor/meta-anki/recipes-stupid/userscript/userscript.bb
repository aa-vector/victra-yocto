DESCRIPTION = "Launch user script hook from data partition"
LICENSE = "Anki-Inc.-Proprietary"
LIC_FILES_CHKSUM = "file://${COREBASE}/../victor/meta-qcom/files/anki-licenses/\
Anki-Inc.-Proprietary;md5=4b03b8ffef1b70b13d869dbce43e8f09"

DEPENDS = "systemd"

FILESPATH =+ "${WORKSPACE}:"
SRC_URI = " \
      file://userscript_run \
      file://userscript.service \
      "

S = "${WORKDIR}/sources"
UNPACKDIR = "${S}"
SYSTEM_DIR = "${D}${sysconfdir}/systemd/system"

do_compile() {
}

do_install() {
   mkdir -p ${D}/usr/sbin
   cp ${S}/userscript_run ${D}/usr/sbin/
   chmod 0755 ${D}/usr/sbin/userscript_run
   if ${@bb.utils.contains('DISTRO_FEATURES', 'systemd', 'true', 'false', d)}; then
      install -d ${SYSTEM_DIR}/
      install -d ${SYSTEM_DIR}/multi-user.target.wants/
      install -m 0644 ${S}/userscript.service -D ${SYSTEM_DIR}/userscript.service
      ln -sf /etc/systemd/system/userscript.service ${SYSTEM_DIR}/multi-user.target.wants/userscript.service
  fi
}

FILES:${PN} += "${systemd_unitdir}/system/ \
		usr/sbin"
