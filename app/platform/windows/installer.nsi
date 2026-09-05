; VitaPlex Windows installer.
;
; Per-user by design, into %LOCALAPPDATA%\Programs\VitaPlex. That choice is not
; laziness about elevation — it is what keeps the in-app updater working. An app
; under Program Files cannot rewrite its own exe without a UAC prompt or a
; privileged helper, and VitaPlex updates itself. Installing per-user means no
; prompt at install, none at update, and none at uninstall.
;
; The portable zip is still built and still supported; this is the same payload
; with a Start Menu entry, an Add/Remove Programs record, and an uninstaller.
;
; Windows will not show a toast from an unpackaged app unless a Start Menu
; shortcut carries the app's AppUserModelID. This installer writes the shortcut;
; the app stamps the id onto it on first run, which also repairs one a user made
; by hand. Doing it there keeps an NSIS plugin off the build.

Unicode true
ManifestDPIAware true

!include "MUI2.nsh"
!include "FileFunc.nsh"

!ifndef VERSION
  !define VERSION "0.0.0.0"
!endif
!ifndef DISPLAY_VERSION
  !define DISPLAY_VERSION "${VERSION}"
!endif
!ifndef SRCDIR
  !define SRCDIR "build"
!endif
!ifndef OUTFILE
  !define OUTFILE "VitaPlex-Setup.exe"
!endif

!define APPNAME     "VitaPlex"
!define PUBLISHER   "Breezyslasher"
!define REGKEY      "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}"

Name "${APPNAME}"
OutFile "${OUTFILE}"
; Per-user: no elevation, at install or uninstall.
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\Programs\${APPNAME}"
; An existing install wins, so an upgrade lands where the last one did.
InstallDirRegKey HKCU "Software\${APPNAME}" "InstallDir"
SetCompressor /SOLID lzma

VIProductVersion "${VERSION}"
VIAddVersionKey "ProductName"     "${APPNAME}"
VIAddVersionKey "CompanyName"     "${PUBLISHER}"
VIAddVersionKey "FileDescription" "${APPNAME} installer"
VIAddVersionKey "FileVersion"     "${VERSION}"
VIAddVersionKey "ProductVersion"  "${DISPLAY_VERSION}"
VIAddVersionKey "LegalCopyright"  "${PUBLISHER}"

!define MUI_ABORTWARNING
!define MUI_ICON   "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"

!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\VitaPlex.exe"
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; Refuse to run over a copy that is still open: overwriting a running exe fails
; part-way and leaves a half-installed directory.
!macro EnsureNotRunning un
Function ${un}EnsureNotRunning
  FindWindow $0 "" "VitaPlex"
  StrCmp $0 0 done
    MessageBox MB_OKCANCEL|MB_ICONEXCLAMATION \
      "VitaPlex is running. Close it and press OK to continue." IDOK done
    Abort
  done:
FunctionEnd
!macroend
!insertmacro EnsureNotRunning ""
!insertmacro EnsureNotRunning "un."

Section "Install"
  Call EnsureNotRunning
  SetOutPath "$INSTDIR"

  File "${SRCDIR}\VitaPlex.exe"
  File "${SRCDIR}\*.dll"
  ; Everything under resources/, recursively — fonts, icons, XML layouts.
  File /r "${SRCDIR}\resources"

  ; The Start Menu entry. It deliberately does NOT set the AppUserModelID here:
  ; that needs an NSIS plugin which is not on a stock runner, and the app stamps
  ; the id onto this shortcut itself on first run (see shortcutHasAumid). Doing
  ; it there rather than here also repairs a shortcut the user made by hand,
  ; which is the case that silently broke toasts before.
  CreateDirectory "$SMPROGRAMS"
  CreateShortcut "$SMPROGRAMS\${APPNAME}.lnk" "$INSTDIR\VitaPlex.exe" "" \
                 "$INSTDIR\VitaPlex.exe" 0 SW_SHOWNORMAL "" "${APPNAME}"

  WriteRegStr HKCU "Software\${APPNAME}" "InstallDir" "$INSTDIR"

  ; Add/Remove Programs. Per-user, so HKCU rather than HKLM.
  WriteRegStr   HKCU "${REGKEY}" "DisplayName"     "${APPNAME}"
  WriteRegStr   HKCU "${REGKEY}" "DisplayVersion"  "${DISPLAY_VERSION}"
  WriteRegStr   HKCU "${REGKEY}" "Publisher"       "${PUBLISHER}"
  WriteRegStr   HKCU "${REGKEY}" "DisplayIcon"     "$INSTDIR\VitaPlex.exe,0"
  WriteRegStr   HKCU "${REGKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr   HKCU "${REGKEY}" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
  WriteRegStr   HKCU "${REGKEY}" "QuietUninstallString" "$\"$INSTDIR\Uninstall.exe$\" /S"
  WriteRegDWORD HKCU "${REGKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${REGKEY}" "NoRepair" 1

  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ; EstimatedSize is in KB and is what Add/Remove Programs shows.
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${REGKEY}" "EstimatedSize" "$0"
SectionEnd

Section "Uninstall"
  Call un.EnsureNotRunning

  Delete "$SMPROGRAMS\${APPNAME}.lnk"
  RMDir /r "$INSTDIR\resources"
  Delete "$INSTDIR\VitaPlex.exe"
  Delete "$INSTDIR\*.dll"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"

  DeleteRegKey HKCU "${REGKEY}"
  DeleteRegKey HKCU "Software\${APPNAME}"

  ; Settings, downloads and cache live in %LOCALAPPDATA%\VitaPlex and are
  ; deliberately left behind: an uninstall is not a request to delete a
  ; library the user spent hours downloading, and a reinstall should find it.
SectionEnd
