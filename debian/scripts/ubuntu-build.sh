#!/bin/bash
# set -x
set -e

# Name of created building folder
BDIR=build-ubuntu
# Package Suffix ...ubuntu<PKG_SFX>+...
PKG_SFX=1

mkdir -p ${BDIR}
rm -rf ${BDIR}/*

PKG_NAME=remmina
PKG_DATE="$(date -R)"
VER_MAJ="$(grep 'set('"${PKG_NAME^^}"'_VERSION_MAJOR' ../Remmina/CMakeLists.txt \
               | sed 's/set('"${PKG_NAME^^}"'_VERSION_[^"]*"//g;s/".*//g')"
VER_MIN="$(grep 'set('"${PKG_NAME^^}"'_VERSION_MINOR' ../Remmina/CMakeLists.txt \
               | sed 's/set('"${PKG_NAME^^}"'_VERSION_[^"]*"//g;s/".*//g')"
VER_REV="$(grep 'set('"${PKG_NAME^^}"'_VERSION_REVISION' ../Remmina/CMakeLists.txt \
               | sed 's/set('"${PKG_NAME^^}"'_VERSION_[^"]*"//g;s/".*//g')"
VER_SFX="$(grep 'set('"${PKG_NAME^^}"'_VERSION_SUFFIX' ../Remmina/CMakeLists.txt \
               | sed 's/set('"${PKG_NAME^^}"'_VERSION_[^"]*"//g;s/".*//g')"


SOURCE_DATE="$(date -u -d "$(cd ../Remmina ; git log -1 --date=iso  | grep '^Date:' | sed 's/^Date: *//g')" "+%Y%m%d%H%M")"
SOURCE_HASH=$(cd ../Remmina ; git log -1 | grep "^commit" | sed "s/^commit *\(.......\).*/\1/g")

DEBSRC_DATE="$(date -u -d "$(git log -1 --date=iso  | grep '^Date:' | sed 's/^Date: *//g')" "+%Y%m%d%H%M")"
DEBSRC_HASH=$(git log -1 | grep "^commit" | sed "s/^commit *\(.......\).*/\1/g")

REMMINA_VERSION="${VER_MAJ}.${VER_MIN}.${VER_REV}"
if [ "${VER_SFX}" ]; then
    REMMINA_VERSION="${REMMINA_VERSION}-${VER_SFX}"
fi

#
#  MAIN
#
# a development release build automatically the new 'changelog' chapter
# an official release use 'changelog' as is except for change serie name
IS_DEV_RELEASE=n
while [ "$1" != "" ]; do
    case $1 in
        -d|--dev)
            IS_DEV_RELEASE=y
            ;;
        -s|--sersfx)
            PKG_SFX=$2
            shift
            ;;
        *)
            usage 1
            ;;
    esac
    shift
done

if [ "$IS_DEV_RELEASE" = "y" ]; then
    REMMINA_PKGVERS="0.9.${DEBSRC_DATE}Ubuntu${PKG_SFX}+git${SOURCE_HASH}+${DEBSRC_HASH}"
    SOURCE_SFX="~dev${SOURCE_DATE}"
    SOURCE_DIR="${PKG_NAME}-${REMMINA_VERSION}${SOURCE_SFX}"
else
    REMMINA_PKGVERS="$(head -n 1 debian/changelog | sed 's/.*-//g;s/).*//g')"
    CHECK_VERSION="$(head -n 1 debian/changelog | sed 's/.*(//g;s/\(.*\)-.*/\1/g')"
    if [ "$CHECK_VERSION" != "$REMMINA_VERSION" ]; then
        echo "Source and changelog version differ: \"$CHECK_VERSION\" != \"$REMMINA_VERSION\"."
        exit 1
    fi
    SOURCE_SFX=""
    SOURCE_DIR="${PKG_NAME}-${REMMINA_VERSION}"
fi
SOURCE_ARCHIVE="${PKG_NAME}_${REMMINA_VERSION}${SOURCE_SFX}.orig.tar.gz"
ABS_SOURCE_ARCHIVE="$PWD/${BDIR}/${SOURCE_ARCHIVE}"

# exports repo without .git folder and other operative system clients
cd ../Remmina
git archive --prefix "$SOURCE_DIR/" --format tar HEAD | gzip -n > "$ABS_SOURCE_ARCHIVE"
cd -
cd "$BDIR"

tar zxvf "${SOURCE_ARCHIVE}"

for serie in yakkety xenial trusty; do
    mv "$SOURCE_DIR" "$PKG_NAME"
    cp -a ../debian "${PKG_NAME}/"
    cd "${PKG_NAME}"

    if [ "$IS_DEV_RELEASE" = "y" ]; then
        cat <<EOF >debian/changelog
${PKG_NAME} (${REMMINA_VERSION}${SOURCE_SFX}-${REMMINA_PKGVERS}~${serie}) ${serie}; urgency=low

  * New upstream release.

 -- ${DEBFULLNAME} <${DEBEMAIL}>  ${PKG_DATE}

EOF
        cat ../../debian/changelog  >>debian/changelog
    else
        sed "1 s/unstable/${serie}/g;s/)/~${serie})/g" < ../../debian/changelog >debian/changelog
    fi
    debuild -eUBUNTU_SERIE="$serie" -S -sa # add ' -us -uc' flags to avoid signing
    cd ..
    rm -rf "${PKG_NAME}"
    tar zxvf "${SOURCE_ARCHIVE}"
done

echo "now cd in ${BDIR} directory and run:"
echo "dput <your-ppa-address> *.changes"

exit 0
