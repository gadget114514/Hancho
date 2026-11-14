# Hancho

## Build

### MSVC ランタイムは静的リンク
- `CMakeLists.txt` で `CMAKE_MSVC_RUNTIME_LIBRARY` を `MultiThreaded`（Debug は `MultiThreadedDebug`）に設定し、VC++ ランタイムを静的リンクしている（`/MT`）。
- そのため `Hancho.exe` / `hancho-cli.exe` は VC++ 再頒布可能パッケージなしで動く。インストーラー（`installer/Hancho.iss`）に `vcruntime140.dll` などを同梱する必要はない。
- リンクする静的ライブラリもすべて `/MT` でビルドすること。`/MD` のライブラリを混ぜると `__imp_strerror` などの未解決シンボルでリンクエラーになる。

### lib/libsixel.lib の作り直し
- `lib/libsixel.lib` は `src/libsixel-windows` から `/MT` でビルドしたもの（同ディレクトリの `CMakeLists.txt` で静的ランタイムを指定済み）。
- 作り直す手順: `src\libsixel-windows\build.bat` を実行し、`src\libsixel-windows\build\Release\libsixel.lib` を `lib\libsixel.lib` にコピーする。
- Release 版しかないため、Debug ビルドでは `LNK4098 (LIBCMT)` の警告が出るが無害。

### バージョン
- バージョンは `CMakeLists.txt` の `project(... VERSION x.y.z)` だけで管理する。C++ の `PC_VERSION`、exe の VERSIONINFO（`src/version_info.rc2`）、インストーラー（`installer/build_installer.cmd` が `/DMyAppVersion` で渡す）はすべてここから決まる。

## リリース

「version x.y.z でリリース」「リリースして」と言われたら、以下を確認を挟まず最後まで順に実行する（この依頼自体が commit、`main` への push、インストール、GitHub リリース公開の許可にあたる）。バージョンの指定がなければ、現在のパッチ番号を 1 上げる。途中で失敗したら、そこで止めて報告する。

1. **事前確認**: `git fetch` して `main` が `origin/main` より遅れていないこと、タグ `vx.y.z` がまだないことを確認する。未コミットの変更はリリースに含める。ただし内容を確認し、無関係なファイルや一時ファイルは含めない（`git add -A` は使わない）。
2. **バージョンとノート**:
   - `CMakeLists.txt` の `project(... VERSION x.y.z)` を更新する。
   - `README.md` と `README-ja.md` の「リリースノート」の先頭に `### vx.y.z` を追加する。ダウンロードは Releases ページへのリンクだけにしているので、個別リリースへのリンクは追加しない。
   - 前回のタグ以降の変更から `dist/release-notes-vx.y.z.md` を書く（形式は `dist` にある前回のファイルに合わせ、英語と日本語の両方を入れる）。
   - これらのファイルは改行コードを保って編集する（`sed -i` は全行の改行を変えることがあるので使わない）。
3. **ビルド**: `cmake --build build --config Release --target Hancho hancho-cli` を実行し、`build\Release\hancho-cli.exe --version` が `x.y.z` を返すことを確認する。
4. **コミットと push**: 変更をコミットして `git push origin main` する。
5. **パッケージ**:
   - `dist\Hancho-vx.y.z-win64\` を新しく作り、次をコピーする: `build\Release` の `Hancho.exe`、`hancho-cli.exe`、`conpty.dll`、`OpenConsole.exe`、およびルートの `LICENSE`、`README.md`、`README-ja.md`、`doc\`、`tools\`、`skills\`。
   - PowerShell の `Compress-Archive -Path <そのフォルダ> -DestinationPath dist\Hancho-vx.y.z-win64.zip` で zip を作る。
   - `installer\build_installer.cmd` で `dist\Hancho-vx.y.z-win64-setup.exe` を作る。
6. **GitHub リリース**: `gh release create vx.y.z <setup.exe> <zip> --target main --title "Hancho vx.y.z" --notes-file dist/release-notes-vx.y.z.md` を実行し、タグが push したコミットを指していること、アセットが 2 つあることを確認する。
7. **インストール**: setup.exe を `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /NOCLOSEAPPLICATIONS /LOG=dist\install-x.y.z.log` で実行する。終了コードが 0 で、`%LOCALAPPDATA%\Programs\Hancho\hancho-cli.exe --version` が `x.y.z` を返すことを確認する。
   - ダッシュボードが起動中だとインストーラーは終了コード 1 で中止する。動作中のジョブを巻き込むおそれがあるので、ダッシュボードは勝手に閉じない。閉じてもらうよう頼み、閉じたら再実行する。

最後に、各手順の結果とリリースの URL を報告する。画面で確認していない変更があれば、そのことも伝える。
