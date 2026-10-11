# Copy to build.local.ps1 (git-ignored) and adjust; build.ps1 loads it before building.

# A skyrim-community-shaders checkout that has been built once (CommonLibSSE-NG 6.7.1 prebuilt + vcpkg tree).
$env:ANIMUNCULI_CS_ROOT = "C:\src\skyrim-community-shaders"

# Where `.\build.ps1 -Deploy` copies the mod (e.g. a Mod Organizer 2 mods folder).
# $env:ANIMUNCULI_DEPLOY_DIR = "C:\Modding\MO2\mods\Animunculi"

# Papyrus compiler setup. With a Creation Kit install in the game folder, SKYRIM_ROOT alone is enough;
# set the individual paths when SKSE's script sources live in a mod manager's folder instead.
# Without a compiler, the build packages the committed data\Scripts\Animunculi.pex.
# $env:SKYRIM_ROOT = "C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition"
# $env:PAPYRUS_SOURCE_ZIP = "$env:SKYRIM_ROOT\Data\Scripts.zip"
# $env:SKSE_SCRIPT_SOURCES = "$env:SKYRIM_ROOT\Data\Scripts\Source"
# $env:PAPYRUS_FLAGS = "$env:SKYRIM_ROOT\Data\Source\Scripts\TESV_Papyrus_Flags.flg"
# $env:PAPYRUS_COMPILER = "$env:SKYRIM_ROOT\Papyrus Compiler\PapyrusCompiler.exe"

# Optional MCPL conformance test against the Claude Code bridge (needs bun).
# $env:MCPL_BRIDGE_DIR = "C:\src\mcpl-cc-bridge\plugins\mcpl-bridge"
