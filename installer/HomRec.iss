; HomRec.iss - Inno Setup script for HomRec's Windows installer/uninstaller.
;
; Build with the Inno Setup Compiler (iscc.exe), from the repo root:
;   iscc /DMyAppVersion=2.0.3 installer\HomRec.iss
; (MyAppVersion defaults to the value baked in below if you omit /D - keep
; that default in sync with src/ui/version.h's HR_APP_VERSION by hand, or
; just always pass /D explicitly. tools/homrec_build.py's optional
; "Installer" step does this for you from version.h automatically.)
;
; Expects hr.exe (and its runtime DLLs, hom.exe, optionally ffmpeg.exe) to
; already be built and sitting in the repo root - this script does not
; compile anything, only packages what's already there. Run `make` (and
; `make hom`) first.
;
; Output: dist\HomRec-Setup-<version>.exe
;
; -- Auto-update -------------------------------------------------------------
; hr.exe's Help > Check for Updates (src/hr_update.cpp) looks at the
; *latest GitHub release* on homaaio/HomREC and, if newer than the running
; version, downloads whichever release asset's filename ends in ".exe" and
; runs it with /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS.
; That means: build this script, upload the resulting Setup exe as an
; asset on the GitHub release tagged vX.Y.Z, and existing installs will
; offer to self-update to it - no separate updater binary needed. The
; AppMutex line below is what lets /CLOSEAPPLICATIONS actually close the
; running hr.exe instead of failing to overwrite it mid-update.

#ifndef MyAppVersion
  #define MyAppVersion "2.0.3"
#endif

#define MyAppName "HomRec"
#define MyAppPublisher "homaaio"
#define MyAppURL "https://github.com/homaaio/HomREC"
#define MyAppExeName "hr.exe"

[Setup]
; Fixed, never-changes-between-versions GUID - this is what lets a newer
; installer recognize "this is an upgrade of an existing HomRec install"
; rather than a side-by-side second copy. Do not regenerate this.
AppId={{7C9C9E2A-6B2B-4B7A-9E2D-3F6D6A7E2C41}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
VersionInfoVersion={#MyAppVersion}

; HomRec keeps its settings (homrec.hrc), logs\, and recordings next to
; hr.exe (see src/hr_log_paths.cpp / hrc_config.h's kDefaultSettingsPath) -
; it was designed as a portable, unzip-anywhere app. Installing to the
; per-user Program Files equivalent keeps that working with zero UAC
; prompts (a machine-wide {autopf} install would need elevation just to
; write its own settings file back out). PrivilegesRequiredOverridesAllowed
; still lets someone deliberately choose an all-users install if they want
; one; they'll just need to run HomRec elevated afterward for it to be
; able to save settings/logs in that case.
DefaultDirName={userpf}\{#MyAppName}
DefaultGroupName={#MyAppName}
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesInstallIn64BitMode=x64compatible

; Detect/close a running HomRec (matches HR_SINGLE_INSTANCE_MUTEX_NAME in
; src/ui/version.h) - needed both for a normal reinstall-over-a-running-app
; and for the silent self-update flow in src/hr_update.cpp.
AppMutex=HomRec_SingleInstance_150
CloseApplications=yes
RestartApplications=no

SourceDir=..
; OutputDir/OutputBaseFilename are NOT affected by SourceDir (unlike
; LicenseFile/SetupIconFile above) - they stay relative to this .iss
; file's own directory (installer\), hence the "..\" to land back in the
; repo-root dist\ that tools/homrec_build.py and its docs both expect.
OutputDir=..\dist
OutputBaseFilename=homrec-setup-win64-{#MyAppVersion}
; Dedicated setup-only icon (separate from icons\main.ico, which is hr.exe's
; own icon) so the installer .exe doesn't look identical to the app itself
; in Explorer/the taskbar while it's running.
SetupIconFile=icons\hrsetup.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
LicenseFile=LICENSE
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
DisableProgramGroupPage=yes
ChangesEnvironment=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
; Both unchecked by default and both exactly mirror the two toggles the
; app's own Settings > System tab offers (settings_dialog.cpp's
; BuildSystemTab()) - see [Icons]/[Registry] below for what each does.
; This is deliberately the *only* place system integration is offered at
; install time now; the first-run Welcome wizard no longer has its own
; copy of this step (see welcome_dialog.cpp).
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked
Name: "autostart"; Description: "&Launch HomRec automatically when Windows starts"; GroupDescription: "System integration:"; Flags: unchecked
; Associates .hrc (settings) and .hrp (plugin package) files with HomRec and
; gives them their own icons (icons\files_icons\hrc.ico / hrp.ico) instead of
; Explorer's generic "unknown file type" icon. Off by default, same as the
; two tasks above - opt-in system integration, not a silent default.
Name: "fileassoc"; Description: "Associate &.hrc and .hrp files with HomRec"; GroupDescription: "System integration:"; Flags: unchecked

[Files]
Source: "hr.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "hom.exe"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
; hr.exe links wxWidgets/etc dynamically (only libgcc/libstdc++/pthread are
; -static, see the Makefile) - the .dll's MinGW drops next to it at build
; time have to travel with it. Same reasoning tools/homrec_build.py uses
; for its "full"/"portable" archive presets.
Source: "*.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "ffmpeg.exe"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "cfg\*"; DestDir: "{app}\cfg"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "plugins\*"; DestDir: "{app}\plugins"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "README.md"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "CHANGELOG.txt"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "SUPPORT.md"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "commands.md"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "LICENSE"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "icons\files_icons\hrc.ico"; DestDir: "{app}\icons\files_icons"; Flags: ignoreversion skipifsourcedoesntexist
Source: "icons\files_icons\hrp.ico"; DestDir: "{app}\icons\files_icons"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{userdesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; Exactly the key/value HrSystemIntegration::SetAutostart(true) writes
; (src/hr_system_integration.cpp) - HKCU so it needs no elevation and so
; the app's own "Launch HomRec when Windows starts" checkbox in
; Settings > System agrees with what the installer did, either direction.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "HomRec"; ValueData: """{app}\{#MyAppExeName}"""; Tasks: autostart; Flags: uninsdeletevalue

; .hrc / .hrp file association (fileassoc task above) - HKCU, same reasoning
; as the autostart key: no elevation needed, and it only affects the
; installing user. ProgIDs/descriptions match hr_register_file_types()'s own
; table in src/hr_app_logic.cpp exactly (HomRec.Profile / HomRec.Plugin) so
; the two mechanisms can never register the same extension under two
; different, conflicting ProgIDs for the same user. Both extensions just
; launch hr.exe with the file as %1 (see win_main.cpp's command-line
; handling for what hr.exe does with it).
Root: HKCU; Subkey: "Software\Classes\.hrc"; ValueType: string; ValueName: ""; ValueData: "HomRec.Profile"; Tasks: fileassoc; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\HomRec.Profile"; ValueType: string; ValueName: ""; ValueData: "HomRec Profile"; Tasks: fileassoc; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\HomRec.Profile\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\icons\files_icons\hrc.ico"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\Classes\HomRec.Profile\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: fileassoc

Root: HKCU; Subkey: "Software\Classes\.hrp"; ValueType: string; ValueName: ""; ValueData: "HomRec.Plugin"; Tasks: fileassoc; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\HomRec.Plugin"; ValueType: string; ValueName: ""; ValueData: "HomRec Plugin Package"; Tasks: fileassoc; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\HomRec.Plugin\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\icons\files_icons\hrp.ico"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\Classes\HomRec.Plugin\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: fileassoc

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: files; Name: "{app}\homrec.hrc"; Check: ShouldDeleteUserData
Type: filesandordirs; Name: "{app}\logs"; Check: ShouldDeleteUserData
Type: filesandordirs; Name: "{app}\Assets"; Check: ShouldDeleteUserData

[Code]
var
  KeepUserDataChoice: Boolean;

function InitializeUninstall(): Boolean;
begin
  KeepUserDataChoice := (MsgBox('Keep your HomRec settings and logs (homrec.hrc, logs\, Assets\)?' + #13#10 +
    'Choose No to remove them along with the program. Your recordings are never touched either way.',
    mbConfirmation, MB_YESNO) = IDYES);
  Result := True;
end;

function ShouldDeleteUserData(): Boolean;
begin
  Result := not KeepUserDataChoice;
end;

// Notifies Explorer that file associations changed (same SHChangeNotify
// call hr_register_file_types() in src/hr_app_logic.cpp makes after
// registering these itself), so a freshly-associated .hrc/.hrp shows its
// new icon right away instead of only after the next Explorer restart.
procedure SHChangeNotify(wEventId: Longint; uFlags: Longint; dwItem1: Longint; dwItem2: Longint);
  external 'SHChangeNotify@shell32.dll stdcall';

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('fileassoc') then
    SHChangeNotify($08000000 { SHCNE_ASSOCCHANGED }, 0, 0, 0);
end;
