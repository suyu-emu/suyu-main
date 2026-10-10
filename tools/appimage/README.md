# Ubuntu 24.04 AppImage

This packages already-built suyu/suyu-cmd; it adds no product dependencies. Compatibility target is Ubuntu24.04x86_64. Older distributions are not promised.

Run from the repository on Ubuntu24.04 with Docker, Python3, binutils, patchelf, desktop-file-utils, squashfs-tools and Qt6 qmake. Enable authenticated Ubuntu deb-src entries matching installed libraries. Existing _pkg contains suyu,suyu-cmd,LICENSE.txt,THIRD-PARTY-NOTICES.txt,LICENSES. Prefer Linux build SDL_SHARED=OFF SDL_STATIC=ON; existingCPMsourcebundle covers pinnedSDL3. Unownedsharedlibraries failclosed rather than being assigned by filename to an Ubuntu package.

    export SOURCE_DATE_EPOCH=$(git show -s --format=%ct HEAD)
    python3 tools/appimage/runtime.py --work-dir _runtime
    python3 tools/appimage/build.py --package-dir _pkg --work-dir _appimage --runtime-dir _runtime --source-revision "$GITHUB_SHA" --release-name "$GITHUB_REF_NAME" --repository "$GITHUB_REPOSITORY" --qmake qmake6
    python3 tests/appimage/smoke.py _appimage/suyu-linux-x86_64.AppImage --report _appimage/smoke.json
    python3 -m unittest discover -s tests/appimage -v

Destinations must be new. Release refs sanitize slashes to hyphens. Tagv0.0.14 emits _appimage/suyu-linux-x86_64.AppImage and _appimage/suyu-v0.0.14-appimage-sources.tar.gz. AppDir and sidecar appimage-provenance.json remain for audit. Source tar topdir is suyu-v0.0.14-dependency-sources; it is separate from existingCPMsourcebundle. No hand-authored source manifest is an input.

Tool pins in tools.lock.json bind officiallinuxdeploy, Qtplugin and appimagetool asset/source/licensehashes. Toolarchives are verified before extraction/execution. Extractedtoolentrypoints and extractedQtplugin avoidFUSE. Separateappimagetool receives an explicitlocallybuiltruntime rather than automaticallyfetchinglatest. Officialreferences: https://docs.appimage.org/packaging-guide/from-source/linuxdeploy-user-guide.html ; https://github.com/linuxdeploy/linuxdeploy-plugin-qt ; https://github.com/AppImage/appimagetool ; https://docs.appimage.org/user-guide/troubleshooting/fuse.html .

Our runtime lock pins Alpinebaseimage ce64758a...,147exactAPKhashes,14correspondingsourcearchives, exactsource/recipecommits and fixedtimestamp. Runtime430be11c9de8e95aebf55232f432fba189735bf17e018e4654e64808c373b561 is944448bytes. Two clean builds matched and an independentbaselineimage replay with hash-lockedAPKs matched. Sourceclosure includes runtime,libfuse3.15.0+suyupinnedpatch,squashfuse0.5.2,musl1.2.5-r11,zlib1.3.2-r1,zstd1.5.6-r2,mimalloc2.1.7-r0 andGCC14.2.0startupsource. APKmetadata's actualaportsbuildcommits are bound, including correspondingpatchrecipes. Linkermap confirms contributedarchives libc,fuse3,mimalloc,z,zstd,squashfuse,squashfuse_ll; noDT_NEEDED. GCC libgcc/eH LOADrecords have no contributedmembers, but GCCstartupsource is conservativelyincluded. Runtimebuild uses atmost2containerCPUs. CI verifies binaryhash/size, recipehash, sourcehashes and installedlibraryrecipecommits.

After deployment payload_sources.py finds every regular.so, requires matchingdpkgoriginal ELFtext/rodata/data/bss (RPATH/debugchanges are expected), maps exactsourcepackageversion, and downloads sources through apt's signedSourcesindices. Full packagecopyrights are copied; references to/usr/share/common-licenses become fulltexts. Unknown/ambiguousowners and missingSourceindices are fatal. collect_sources.py independentlyverifies runtime receipt against committedlock and covers everyactual.so hash; everysourcearchive is rehashed before deterministic sourcebundle creation. AppDirmtimes/source-tardates use SOURCE_DATE_EPOCH. This is pinned-input reproducibility, not a claim that differentUbuntuAPTupdates produce identicalAppImages.

Embedded usr/share/suyu/appimage-provenance.json schema suyu-appimage-v1 includes runtimeSHA/size/sourceURL/recipeSHA/lockSHA, sourcecommit and exhaustivefiles/symlinkinventory excludingitself. Embedded distribution-sources.json includes runtimebuildreceipt and exactpayloadsource mappings. Scanner must use independentcommittedruntime/source lock, not trust embeddedselfassertions. AppRun is a regularshellscript; defaultGUI or --suyu-cmd CLI; rootapprun-hooks/linuxdeploy-plugin-qt-hook.sh establishespluginpaths; XDG_CURRENT_DESKTOP defaults empty and libqoffscreen.so is explicitly deployed alongside platforminputcontexts. NonFUSEsmoke extractsimage, runs CLIhelp and checks8secondoffscreenGUIstartup; ownedSIGTERM is explicitlynotcleanGUIshutdown proof.

FocusedPythonchecks pass locally. Linuxnativeproductpackaging, fullQtdeploymentlayout, extractedproductsmoke and finalpolicy scans remain CIchecks; no nativegame is part of packaging. Runtimebuild/replayproofs are retained externally in G:/sxdb/appimage-verification, not shipped.

Corresponding-source delivery preserves complete upstream source archives and the
immutable runtime input/build receipt. `source-delivery.lock.json` separately
records exact aports recipe projections: every package-local APKBUILD, patch and
auxiliary file at the installed package's recorded build commit, plus upstream
context documents. Unrelated repository trees are omitted from these explicitly
declared derived archives; the generic snapshot is context-only, not a libfuse
recipe. Original input URLs, snapshot commits and SHA256 remain in `sources`;
physical derived archive hashes and exact members appear in `source_distributions`.
The collector and scanner verify these against the independent delivery lock.

Full upstream archives can contain deliberately malformed decoder fixtures,
source aliases and static test snapshots whose names resemble release userdata.
`source-fixtures.lock.json` admits only independently reviewed exact archive and
ancestor hashes, raw member occurrences, types, targets, sizes and content hashes.
Malformed archive parsing stops only at specifically recorded fixture boundaries;
ordinary raw-content scans still run. Key text, game containers and content
signatures are never excused. The sole key-name admission is the hash-bound
OpenSSL explanatory prose document, not a key payload. Changed bytes, targets,
paths, occurrences, ancestor archives or source manifests fail the gate.

CI diagnostic FUSE-free startup may run after a corresponding-source scan fails
if construction succeeded. That failure still blocks uploads and publication;
diagnostic startup is not a release qualification waiver.


Pinned deployment layout controls: DISABLE_COPYRIGHT_FILES_DEPLOYMENT=1 is supported in linuxdeploy cc7b86472 src/main.cpp114–117 and Qtplugin ce5291e src/main.cpp139–142. This suppresses duplicate tool copyright trees; automatic collector still supplies complete canonical Ubuntu copyright/full common-license texts. Qtplugin src/deployment.h119–151 has no translation-disable flag. The builder validates its optional usr/translations subtree contains only flat regular.qm files, records their hashes, and moves that newlyowned output to _appimage/omitted-qt-translations before releaseinventory. No originalQt/source/license file is deleted or modified; unexpectedfiles/symlinks fail without moving. Omitted optional Qt translations are not releasepayload; product translations embedded in suyu are unaffected. Eight focusedLinux tests pass, including actuallauncherhook and safeoptionaltranslationretention.
