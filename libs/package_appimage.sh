#!/bin/bash
set -e

cp -r linux64 nekobox.AppDir

# The file for Appimage

mv nekobox.AppDir/launcher nekobox.AppDir/updater

cat >nekobox.AppDir/nekobox.desktop <<-EOF
[Desktop Entry]
Name=nekobox
Exec=echo "nekobox started"
Icon=nekobox
Type=Application
Categories=Network
EOF

cat >nekobox.AppDir/AppRun <<-EOF
#!/bin/bash
echo "PATH: \${PATH}"
echo "nekobox runing on: \$APPDIR"
LD_LIBRARY_PATH=\${APPDIR}/usr/lib QT_PLUGIN_PATH=\${APPDIR}/usr/plugins \${APPDIR}/nekobox -appdata "\$@"
EOF

chmod +x nekobox.AppDir/AppRun

# build

curl -fLSO --retry 3 https://github.com/AppImage/appimagetool/releases/latest/download/appimagetool-x86_64.AppImage
chmod +x appimagetool-x86_64.AppImage
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 ./appimagetool-x86_64.AppImage nekobox.AppDir nekobox-x86_64.AppImage

# clean

rm appimagetool-x86_64.AppImage
rm -rf nekobox.AppDir
