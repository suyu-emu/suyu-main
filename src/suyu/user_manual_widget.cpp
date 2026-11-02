// SPDX-FileCopyrightText: 2024 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QLineEdit>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDesktopServices>
#include <QFrame>
#include <QLabel>
#include <QSplitter>
#include <QTextCursor>
#include <QUrl>

#ifdef SUYU_USE_QT_WEB_ENGINE
#include <QWebEngineView>
#endif

#include "suyu/user_manual_widget.h"

UserManualWidget::UserManualWidget(QWidget* parent) : QWidget(parent, Qt::Window) {
    setWindowTitle(QStringLiteral("User Manual"));
    resize(1100, 820);
    SetupUi();
    LoadDefaultContent();
    LoadDocsUrl(QUrl(QStringLiteral("https://suyu-emu.github.io/website/docs")));
}

UserManualWidget::~UserManualWidget() = default;

void UserManualWidget::SetupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    // Toolbar
    auto* toolbar = new QHBoxLayout();
    btn_back_ = new QPushButton(QStringLiteral("<"), this);
    btn_forward_ = new QPushButton(QStringLiteral(">"), this);
    btn_reload_ = new QPushButton(tr("Reload Docs"), this);
    btn_open_external_ = new QPushButton(tr("Open Docs in Browser"), this);
    search_box_ = new QLineEdit(this);
    search_box_->setPlaceholderText(QStringLiteral("Search built-in guide..."));
    docs_status_label_ = new QLabel(this);
    docs_status_label_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    docs_status_label_->setWordWrap(false);

    toolbar->addWidget(btn_back_);
    toolbar->addWidget(btn_forward_);
    toolbar->addWidget(btn_reload_);
    toolbar->addWidget(btn_open_external_);
    toolbar->addWidget(docs_status_label_, 1);
    toolbar->addWidget(search_box_);
    layout->addLayout(toolbar);

    auto* intro_label = new QLabel(
        tr("Live docs are embedded above when WebEngine is available. The built-in guide below stays available offline and covers keys, game setup, Export Game, Steam shortcuts, Nintendo account sync, and troubleshooting."),
        this);
    intro_label->setWordWrap(true);
    intro_label->setStyleSheet(QStringLiteral("color: palette(mid);"));
    layout->addWidget(intro_label);

    content_splitter_ = new QSplitter(Qt::Vertical, this);

#ifdef SUYU_USE_QT_WEB_ENGINE
    docs_view_ = new QWebEngineView(content_splitter_);
    docs_view_->setUrl(QUrl(QStringLiteral("about:blank")));
    content_splitter_->addWidget(docs_view_);
#else
    auto* docs_fallback = new QFrame(content_splitter_);
    auto* docs_fallback_layout = new QVBoxLayout(docs_fallback);
    auto* docs_fallback_label = new QLabel(
        tr("Qt WebEngine is not available in this build, so the online docs cannot be embedded here. Use the button above to open the public docs site in your browser."),
        docs_fallback);
    docs_fallback_label->setWordWrap(true);
    docs_fallback_label->setAlignment(Qt::AlignCenter);
    docs_fallback_layout->addStretch();
    docs_fallback_layout->addWidget(docs_fallback_label);
    docs_fallback_layout->addStretch();
    content_splitter_->addWidget(docs_fallback);
#endif

    browser_ = new QTextBrowser(content_splitter_);
    browser_->setOpenExternalLinks(true);
    browser_->setOpenLinks(true);
    content_splitter_->addWidget(browser_);
    content_splitter_->setStretchFactor(0, 3);
    content_splitter_->setStretchFactor(1, 2);
    content_splitter_->setChildrenCollapsible(false);
    layout->addWidget(content_splitter_, 1);

    // Connections
#ifdef SUYU_USE_QT_WEB_ENGINE
    connect(btn_back_, &QPushButton::clicked, this, [this] {
        if (docs_view_ != nullptr) {
            docs_view_->back();
        }
    });
    connect(btn_forward_, &QPushButton::clicked, this, [this] {
        if (docs_view_ != nullptr) {
            docs_view_->forward();
        }
    });
    connect(btn_reload_, &QPushButton::clicked, this, [this] {
        if (docs_view_ != nullptr) {
            docs_view_->reload();
        }
    });
    connect(docs_view_, &QWebEngineView::urlChanged, this, [this](const QUrl& url) {
        docs_status_label_->setText(url.toString());
    });
#else
    connect(btn_back_, &QPushButton::clicked, browser_, &QTextBrowser::backward);
    connect(btn_forward_, &QPushButton::clicked, browser_, &QTextBrowser::forward);
    connect(btn_reload_, &QPushButton::clicked, this, [this] {
        LoadDocsUrl(docs_url_);
    });
#endif
    connect(btn_open_external_, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(docs_url_);
    });
    connect(search_box_, &QLineEdit::textChanged, this, &UserManualWidget::OnSearchTextChanged);

    setLayout(layout);
}

void UserManualWidget::LoadDefaultContent() {
    browser_->setHtml(QStringLiteral(
        "<h1>suyu Manual</h1>"
        "<p><b>What you are looking at:</b> the top pane loads the public Suyu docs site inside the app when WebEngine support is available. This lower pane keeps the local reference guide available even if the site is offline or the current build does not ship WebEngine.</p>"
        "<h2>Quick Start</h2>"
        "<ol>"
        "<li>In Gamer mode, open <b>Library</b> and click <b>Add a Game</b> to add a game directory, or use <b>File → Load File</b> for a single ROM/NSP file.</li>"
        "<li>Install <b>prod.keys</b> from <b>Tools → Install Decryption Keys</b>. You can also configure an external decryption tool if you prefer that workflow.</li>"
        "<li>Use the game library to select and launch your game. The gamer grid and list view both support search and context menus.</li>"
        "<li>Configure input from <b>Emulation → Configure → Controls</b>.</li>"
        "<li>Visit <b>Emulation → Configure → Graphics</b> to tune performance.</li>"
        "</ol>"
        "<h2>Keys, Firmware, and Decryption</h2>"
        "<p>Built-in key installation is available again from <b>Tools → Install Decryption Keys</b>. Place <b>prod.keys</b> and optional <b>title.keys</b> there to restore local decryption. External decryption remains available, but it is no longer the only path.</p>"
        "<ul>"
        "<li>If a title still reports missing firmware or decryption, verify the keys are in the Suyu keys directory and restart the scan.</li>"
        "<li>If you update keys while the library is open, suyu now refreshes the content providers and repopulates the game list safely.</li>"
        "<li>Install firmware from <b>Tools → Install Firmware</b>. Exported games use the keys and firmware installed here; see <b>Keys and Firmware in Exported Games</b> below.</li>"
        "</ul>"
        "<h2>Game Library</h2>"
        "<p>The library view shows imported games, allows sorting, and offers search by title or file path. Drag-and-drop support makes it easy to add games. Gamer mode also pulls cover art from online sources and caches it for reuse.</p>"
        "<h2>Nintendo Account</h2>"
        "<p>The Nintendo Account dialog supports browser-based sign-in when Qt WebEngine is present. Linked accounts can verify the stored session token and fetch the Nintendo web purchase history used for owned-title indicators.</p>"
        "<ul>"
        "<li>Use <b>One-Click Sign In</b> for the streamlined path.</li>"
        "<li>If embedded sign-in is unavailable in your build, the same flow can still be completed in your external browser and pasted back into the dialog.</li>"
        "</ul>"
        "<h2>Steam Integration</h2>"
        "<p>suyu detects Steam automatically and can add your games as non-Steam shortcuts. This allows launcher and overlay support without requiring a Steam API key.</p>"
        "<p>Use the Steam shortcut action from a game entry to add or update a shortcut that launches the selected game through suyu. To add an exported game instead, tick <b>Add to Steam library when the export finishes</b> in the Export Game dialog (see below).</p>"
        "<p>To add a game to Steam:</p>"
        "<ul>"
        "<li>Right-click a game entry and choose the Steam shortcut action.</li>"
        "<li>suyu uses the saved artwork preference and can add the shortcut even when artwork is unavailable.</li>"
        "<li>Steam shortcuts are written directly into your local Steam userdata configuration. Your other shortcuts are kept as they were, and the file is left untouched if suyu cannot fully read it.</li>"
        "<li>Shortcuts go to the first Steam account found. Restart Steam to see changes.</li>"
        "</ul>"
        "<h2>Export Game</h2>"
        "<p>Use <b>File → Export Game</b> to turn a game into its own package. On Windows, a <b>Build</b> export is a program you can run without opening suyu. Linux and macOS exports are Source only, with no bundled runtime.</p>"
        "<p><b>Re-export builds made with v0.0.10 or earlier.</b> Older (ABI 4) static and Hybrid builds are refused.</p>"
        "<p>Static and Hybrid exports are now much faster: on Windows a Mario Kart 8 Deluxe race goes from about 24 to about 56 fps (about 60 on macOS and Linux). On Windows, reaching that speed needs Clang/LLVM installed (<b>winget install LLVM.LLVM</b>, or Visual Studio's <b>C++ Clang tools for Windows</b>); without it the export still works, using Microsoft's compiler, but runs slower. <b>Re-export a game you already exported to get the speed-up.</b> Static is still tested on Mario Kart 8 Deluxe only; the Dynarmic JIT stays the default and the most compatible choice.</p>"
        "<ul>"
        "<li><b>CPU Backend:</b> <b>suyu Dynarmic JIT (Baseline)</b> is the default and the most compatible (Windows only). <b>suyu Hybrid JIT + AOT</b> runs pre-compiled code with a JIT fallback; performance varies by game, so compare it with the Dynarmic JIT export. <b>suyu static AOT (Experimental)</b> has no fallback and is for testing. The dialog shows a coverage line for the selected game, such as whether static AOT looks safe to try; playing a Hybrid export records code that still needed the JIT, and re-exporting later can cover more of it. <b>Import / Export coverage file</b> lets players share this (module IDs, code offsets and counts only, no game code). Hybrid remains the choice when static won't run a game.</li>"
        "<li><b>Export Format:</b> <b>Build</b> is the default on Windows. <b>Source</b> writes a C project to compile yourself.</li>"
        "<li><b>Update:</b> this row shows which game update the export will use, with a <b>From:</b> line showing where it comes from. <b>Install Update File...</b> installs an update .nsp after checking it belongs to the game. If no update is installed, exporting asks whether to install one or export without it.</li>"
        "<li><b>Output:</b> defaults to the <b>exports</b> folder of a source checkout, otherwise your Downloads folder.</li>"
        "<li>Packages copy your suyu settings (graphics, CPU, audio, system, applets), without debug settings, paths, input devices or personal data. <b>Include custom game configuration</b> adds the game's own settings on top.</li>"
        "<li><b>Include transferable shader cache</b> carries the shaders suyu has already built, which reduces stutter the first time you play the package. Ticking save data, shader cache or custom configuration now reliably includes them; older exports could leave them out even when ticked.</li>"
        "<li>The progress bar and status line follow the real work, for example <i>Compiling 312/940 files (main)</i>. Each launch, an exported game prepares the shaders in its cache with its own progress bar (<i>Preparing shaders: N of M</i>) and shows <i>Building shaders N/M</i> in the title bar; this is normal and usually takes a few seconds.</li>"
        "</ul>"
        "<p><b>Steam:</b> tick <b>Add to Steam library when the export finishes</b> (needs Build, or the Dynarmic JIT backend). The shortcut runs the export's own program and is named after the game and backend, for example <i>(suyu Dynarmic JIT)</i>. <b>...and replace an existing shortcut</b> removes only suyu's own shortcut for that game. Artwork is made from the game icon; <b>...and fetch cover art from Wikipedia</b> sends the game title to Wikipedia to get cover art instead. Restart Steam to see the shortcut. Steam shows no description for non-Steam games.</p>"
        "<p><b>Discord:</b> exported games show up in Discord as playing suyu, with the game's cover art from Wikipedia (which receives the game's title). This is on by default; untick <b>Show this game in Discord (cover art from Wikipedia)</b> before exporting to turn it off, or set <b>enabled=0</b> in the <b>discord.ini</b> file next to the game's .exe afterward. In the suyu launcher, turn it off from <b>Configure → Web → Show Current Game in your Discord Status</b>.</p>"
        "<h2>Keys and Firmware in Exported Games</h2>"
        "<p>Exports never contain keys or firmware. An exported game reads them from the suyu installed on the same computer: <b>%APPDATA%\\suyu\\keys</b> and <b>%APPDATA%\\suyu\\nand</b> on Windows, or the <b>user</b> folder of a portable suyu. A custom NAND folder is honoured.</p>"
        "<ul>"
        "<li>If <b>prod.keys</b> is missing, the game shows the exact folder and offers <b>Install keys</b>, <b>Open folder</b> and <b>Quit</b>. Copying prod.keys into that folder works just as well.</li>"
        "<li>If firmware is missing, the game warns and offers <b>Continue anyway</b>. Mii screens and some menus may fail without firmware.</li>"
        "</ul>"
        "<h2>Controllers and Input</h2>"
        "<p>Configure controllers in the Controls settings. Both physical controllers and keyboard layouts can be mapped. "
        "If a controller is not recognized, reconnect it and restart the emulator.</p>"
        "<h3>Controllers in Exported Games</h3>"
        "<ul>"
        "<li>The first gamepad becomes Player 1; more pads become Players 2-8 in the order they connect, as Pro Controllers.</li>"
        "<li>Unplugging a pad disconnects that player; plugging it back in returns it to the same slot (Players 2–8). If all pads leave, the keyboard returns to Player 1.</li>"
        "<li>Left and right Joy-Con halves combine into one player. Press <b>F12</b> for the controls panel, including <b>Combine Joy-Cons into one player</b> and <b>Split Joy-Cons into two players</b>.</li>"
        "<li>The F12 panel also has a <b>Resolution Scale</b> choice; it's saved in the package and takes effect the next time the game starts.</li>"
        "<li>Choosing <b>Use keyboard</b> or <b>Rebind</b> in the F12 panel turns automatic assignment off for that package.</li>"
        "</ul>"
        "<h2>Graphics and Audio</h2>"
        "<p>Use the Graphics settings to select the renderer, toggle VSync, and adjust resolution scaling. "
        "Audio settings are available in the Audio panel for volume and output device selection.</p>"
        "<h2>Saving Progress</h2>"
        "<p>This Qt frontend does not expose Save State or Load State toolbar actions. Use the game's own save features.</p>"
        "<h2>Networking and Multiplayer</h2>"
        "<p>If multiplayer features are enabled, use the dedicated room server to host or join sessions. "
        "Multiplayer rooms can be announced through the web service when available.</p>"
        "<h2>Troubleshooting</h2>"
        "<p>If the emulator fails to start, use <b>File → Open suyu Folder</b> and inspect its <b>log</b> directory.</p>"
        "<ul>"
        "<li>Missing games in gamer mode often means the source path was not added to the library scan list yet.</li>"
        "<li>Owned Nintendo titles are informational until the corresponding ROM or dumped title is available locally.</li>"
        "<li>Adding a game shortcut depends on a detected Steam userdata folder. If Steam is portable or installed in a custom location, confirm the Steam path is readable.</li>"
        "</ul>"
        "<h3>Exported Games</h3>"
        "<ul>"
        "<li><b>Missing keys or firmware dialog:</b> install them in suyu (<b>Tools → Install Decryption Keys</b> / <b>Install Firmware</b>), or copy prod.keys into the folder the dialog shows, then start the game again.</li>"
        "<li><b>An exported game stutters:</b> play the game in suyu first, then re-export with <b>Include transferable shader cache</b> — the export includes your existing cache. The title bar shows <i>Building N shaders</i> when a stutter comes from a new shader; this is normal on first play of a new area and goes away as the cache fills.</li>"
        "<li><b>The game shows Ver. 1.0.0:</b> the package was made before v0.0.11. Re-export it.</li>"
        "<li><b>A Steam shortcut does not appear:</b> restart Steam.</li>"
        "<li><b>Rare crash at start-up:</b> just launch the game again. Closing suyu while the Export Game dialog is open can also crash suyu.</li>"
        "<li><b>No sound in an exported game:</b> check <b>audio_muted</b> in the package's <b>user\\config\\sdl2-config.ini</b> and set it to <b>false</b>.</li>"
        "</ul>"
        "<h2>Advanced Tips</h2>"
        "<ul>"
        "<li>Use the embedded docs pane for the public online docs and this lower guide for suyu-specific behavior.</li>"
        "<li>For the best performance, enable the correct renderer for your GPU.</li>"
        "<li>Keep your Steam client up to date to ensure compatibility with Steam shortcut import.</li>"
        "</ul>"
        "<h2>Additional Resources</h2>"
        "<ul>"
        "<li><a href='https://suyu-emu.github.io/website/docs'>Public Suyu docs site</a></li>"
        "<li><a href='qrc:/docs/README.md'>Project docs resource paths</a> if your build packages them</li>"
        "</ul>"));
}

void UserManualWidget::LoadContent(const QString& resource_path) {
    const QUrl url(resource_path);
    if (url.isValid() && !url.scheme().isEmpty() &&
        (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https"))) {
        LoadDocsUrl(url);
        return;
    }

    browser_->setSource(url);
}

void UserManualWidget::LoadDocsUrl(const QUrl& url) {
    docs_url_ = url;
    docs_status_label_->setText(url.toString());

#ifdef SUYU_USE_QT_WEB_ENGINE
    if (docs_view_ != nullptr) {
        docs_view_->setUrl(url);
    }
#endif
}

void UserManualWidget::OnSearchTextChanged(const QString& text) {
    if (text.isEmpty()) {
        browser_->moveCursor(QTextCursor::Start);
        return;
    }
    browser_->find(text);
}
