; Haul installer (NSIS 3, Modern UI 2)
;   makensis /DVERSION=0.2.1 /DSTAGE=..\..\stage /DOUTFILE=Haul-0.2.1-Setup.exe installer.nsi

Unicode true
!include "MUI2.nsh"

!ifndef VERSION
  !define VERSION "0.2.1"
!endif
!ifndef STAGE
  !define STAGE "..\..\stage"
!endif
!ifndef OUTFILE
  !define OUTFILE "Haul-${VERSION}-Setup.exe"
!endif

!define APPNAME "Haul"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Haul"

Name "${APPNAME} ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\${APPNAME}"
InstallDirRegKey HKLM "Software\${APPNAME}" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APPNAME}"
VIAddVersionKey "FileDescription" "${APPNAME} installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "MIT License"

!define MUI_ICON "haul.ico"
!define MUI_UNICON "haul.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\bin\haul.exe"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "Haul" SecMain
  SectionIn RO
  ; Upgrade: remove the previous program files first (user data lives in %APPDATA% and is kept).
  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\share"

  SetOutPath "$INSTDIR"
  File /r "${STAGE}\*.*"

  WriteRegStr HKLM "Software\${APPNAME}" "InstallDir" "$INSTDIR"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\bin\haul.exe"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk" "$INSTDIR\Uninstall.exe"

  WriteRegStr HKLM "${UNINSTKEY}" "DisplayName" "${APPNAME}"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINSTKEY}" "Publisher" "Haul contributors"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\bin\haul.exe"
  WriteRegStr HKLM "${UNINSTKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "${UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegStr HKLM "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTKEY}" "URLInfoAbout" "https://github.com/tehsaad/Haul"
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  DeleteRegKey HKCU "Software\Google\Chrome\NativeMessagingHosts\io.github.tehsaad.haul"
  DeleteRegKey HKCU "Software\Microsoft\Edge\NativeMessagingHosts\io.github.tehsaad.haul"
  DeleteRegKey HKCU "Software\BraveSoftware\Brave-Browser\NativeMessagingHosts\io.github.tehsaad.haul"
  DeleteRegKey HKCU "Software\Mozilla\NativeMessagingHosts\io.github.tehsaad.haul"
  Delete "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk"
  RMDir "$SMPROGRAMS\${APPNAME}"
  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\share"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  ; Launch-at-login entry, if the user enabled it in Settings.
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "Haul"
  DeleteRegKey HKLM "${UNINSTKEY}"
  DeleteRegKey HKLM "Software\${APPNAME}"
  ; Downloads, history (%APPDATA%\Haul) and settings (HKCU\Software\Haul) are kept.
SectionEnd
