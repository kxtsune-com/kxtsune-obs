OutFile "kxtsune-obs-setup.exe"

Unicode true
RequestExecutionLevel user

SetDatablockOptimize on
SetCompress auto
SetCompressor /SOLID lzma

Name "kxtsune-obs"
Caption "Multiple RTMP Output Plugin for OBS Studio"
Icon "${NSISDIR}\Contrib\Graphics\Icons\win-install.ico"

Var /Global DefInstDir
Function .onInit
    ReadEnvStr $0 "ALLUSERSPROFILE"
    StrCpy $DefInstDir "$0\obs-studio\plugins"
    StrCpy $INSTDIR "$DefInstDir"

    IfFileExists "$DefInstDir\kxtsune-obs\*.*" AskUninst DontAskUninst
    AskUninst:
        MessageBox MB_YESNO|MB_ICONQUESTION "Install or remove kxtsune-obs? Yes = Install, No = Remove" IDYES NotDoUninst IDNO DoUninst
    DoUninst:
        RMDir /r "$DefInstDir"
        MessageBox MB_OK|MB_ICONINFORMATION "完成$\r$\n$\r$\n完了$\r$\n$\r$\nDone"
        Quit
    NotDoUninst:
    DontAskUninst:
FunctionEnd

Function onDirPageLeave
StrCmp "$INSTDIR" "$DefInstDir" DirNotModified DirModified
DirModified:
MessageBox MB_OK|MB_ICONSTOP "Please don't change the install directory."
Abort
DirNotModified:
FunctionEnd

Page directory "" "" onDirPageLeave
Page instfiles

Section
SetOutPath "$INSTDIR"
File /r "release\Release\kxtsune-obs"
SectionEnd

