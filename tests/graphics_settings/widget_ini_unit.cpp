// SPDX-License-Identifier: GPL-2.0-or-later
// Native Qt fixture: link the production shared widgets, translations, QtConfig and common code.
#include <cstdlib>
#include <iostream>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStringList>
#include <QSlider>
#include <QTemporaryDir>
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "suyu/configuration/configure_graphics_advanced.h"
#include "suyu/configuration/qt_config.h"
#include "suyu/configuration/shared_widget.h"
#include "ui_configure_graphics_advanced.h"

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

static void CheckAdvancedCategories() {
    QWidget page;
    Ui::ConfigureGraphicsAdvanced ui;
    ui.setupUi(&page);
    ConfigurationShared::Builder builder(&page, true);
    bool hacks_present = false;
    bool extensions_present = false;
    const auto translations = ConfigurationShared::InitializeTranslations(&page);
    std::vector<std::function<void(bool)>> ignored_apply;
    for (const auto category : ConfigureGraphicsAdvanced::SettingsCategories) {
        hacks_present |= category == Settings::Category::RendererHacks;
        extensions_present |= category == Settings::Category::RendererExtensions;
        for (auto* setting : Settings::values.linkage.by_category[category]) {
            auto* widget = builder.BuildWidget(setting, ignored_apply);
            Check(widget && widget->Valid(), setting->GetLabel().c_str());
            ui.populate_target->layout()->addWidget(widget);
            if (setting->Id() == Settings::values.enable_compute_pipelines.Id()) {
                widget->hide();
            }
            Check(translations->contains(setting->Id()) &&
                      !translations->at(setting->Id()).first.isEmpty(), "descriptive advanced label");
            if (setting->IsEnum()) {
                Check(widget->combobox && widget->combobox->currentIndex() >= 0,
                      "advanced enum choices include configured value");
            }
        }
    }
    Check(hacks_present && extensions_present, "active advanced category coverage");
    page.resize(720, 420);
    page.show();
    QApplication::processEvents();
    Check(page.minimumSizeHint().height() <= 420, "advanced page permits compact dialog height");
    Check(ui.advanced_scroll_area->verticalScrollBar()->maximum() > 0,
          "advanced settings scroll at compact height");
    ui.advanced_scroll_area->verticalScrollBar()->setValue(
        ui.advanced_scroll_area->verticalScrollBar()->maximum());
    const auto check_choices = [&](Settings::BasicSetting* setting, const QStringList& labels) {
        auto* widget = builder.BuildWidget(setting, ignored_apply);
        Check(widget && widget->combobox && widget->combobox->count() == labels.size(),
              "all graphics hack enum choices");
        for (int index = 0; index < labels.size(); ++index) {
            Check(widget->combobox->itemText(index) == labels[index], "graphics hack enum units");
        }
    };
    check_choices(&Settings::values.fast_gpu_time,
                  {QStringLiteral("Normal (no divisor)"), QStringLiteral("Medium (256)"),
                   QStringLiteral("High (512)")});
    check_choices(&Settings::values.gpu_unswizzle_texture_size,
                  {QStringLiteral("16 MiB"), QStringLiteral("32 MiB"), QStringLiteral("128 MiB"),
                   QStringLiteral("256 MiB"), QStringLiteral("512 MiB")});
    check_choices(&Settings::values.gpu_unswizzle_stream_size,
                  {QStringLiteral("4 MiB"), QStringLiteral("8 MiB"), QStringLiteral("16 MiB"),
                   QStringLiteral("32 MiB"), QStringLiteral("64 MiB")});
    check_choices(&Settings::values.gpu_unswizzle_chunk_size,
                  {QStringLiteral("32 slices"), QStringLiteral("64 slices"),
                   QStringLiteral("128 slices"), QStringLiteral("256 slices"),
                   QStringLiteral("512 slices")});
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    Check(temporary.isValid(), "temporary config directory");
    Common::FS::SetSuyuPath(Common::FS::SuyuPath::ConfigDir,
                          temporary.path().toStdString());
    Settings::SetConfiguringGlobal(true);
    {
        QtConfig clean("graphics-clean-defaults");
        Check(!Settings::values.use_graphics_pipeline_library.GetValue(), "fresh GPL is opt-in");
        Check(!Settings::values.use_fast_gpu_time.GetValue(), "fresh fast GPU time is opt-in");
        Check(!Settings::values.use_asynchronous_shaders.GetValue(), "fresh async shaders is opt-in");
        Check(!Settings::values.optimize_spirv_output.GetValue(), "fresh SPIR-V optimization is opt-in");
        Check(!Settings::values.enable_compute_pipelines.GetValue(), "fresh Intel compute is opt-in");
        Check(!Settings::values.disable_shader_loop_safety_checks.GetValue(), "shader loop safety retained");
        Check(Settings::values.use_disk_shader_cache.GetValue(), "disk shader cache retained");
        Check(Settings::values.use_vulkan_driver_pipeline_cache.GetValue(), "driver cache retained");
    }
    // Historical values marked as defaults must adopt the new conservative defaults.
    {
        QSettings legacy(temporary.path() + QStringLiteral("/graphics-fixture.ini"),
                         QSettings::IniFormat);
        for (const auto& key : {QStringLiteral("use_graphics_pipeline_library"),
                               QStringLiteral("use_fast_gpu_time"),
                               QStringLiteral("vertex_input_dynamic_state")}) {
            legacy.setValue(QStringLiteral("Renderer/") + key, true);
            legacy.setValue(QStringLiteral("Renderer/") + key + QStringLiteral("/default"), true);
        }
        legacy.setValue(QStringLiteral("Renderer/dyna_state"), 3);
        legacy.setValue(QStringLiteral("Renderer/dyna_state/default"), true);
        legacy.sync();
    }
    QtConfig global("graphics-fixture");
    Check(!Settings::values.use_graphics_pipeline_library.GetValue(), "legacy GPL default resets");
    Check(!Settings::values.use_fast_gpu_time.GetValue(), "legacy fast GPU default resets");
    Check(Settings::values.vertex_input_dynamic_state.ToString() ==
              Settings::values.vertex_input_dynamic_state.DefaultToString(), "legacy vertex default resets");
    Check(Settings::values.dyna_state.ToString() == Settings::values.dyna_state.DefaultToString(),
          "legacy dynamic default resets");
    QWidget parent;
    ConfigurationShared::Builder builder(&parent, true);
    CheckAdvancedCategories();
    std::vector<std::function<void(bool)>> apply;
    auto* pipeline = builder.BuildWidget(&Settings::values.use_graphics_pipeline_library, apply);
    auto* fast_gpu = builder.BuildWidget(&Settings::values.use_fast_gpu_time, apply);
    auto* dynamic = builder.BuildWidget(&Settings::values.dyna_state, apply);
    auto* vertex = builder.BuildWidget(&Settings::values.vertex_input_dynamic_state, apply);
    auto* samples = builder.BuildWidget(&Settings::values.sample_shading, apply,
                                       ConfigurationShared::RequestType::Default, true,
                                       ConfigurationShared::default_multiplier, nullptr,
                                       QStringLiteral("%"));
    Check(pipeline && pipeline->Valid() && pipeline->checkbox, "pipeline checkbox");
    Check(fast_gpu && fast_gpu->Valid() && fast_gpu->checkbox, "fast GPU checkbox");
    Check(dynamic && dynamic->Valid() && dynamic->combobox, "dynamic state combo");
    Check(dynamic->combobox->count() == 4, "all dynamic state enum choices");
    Check(dynamic->combobox->itemText(0) == QStringLiteral("Disabled") &&
              dynamic->combobox->itemText(1) == QStringLiteral("EDS1") &&
              dynamic->combobox->itemText(2) == QStringLiteral("EDS2") &&
              dynamic->combobox->itemText(3) == QStringLiteral("EDS3"), "dynamic state labels");
    Check(vertex && vertex->Valid() && vertex->checkbox, "vertex checkbox");
    Check(samples && samples->Valid() && samples->slider, "sample shading slider");
    Check(samples->slider->minimum() == 0 && samples->slider->maximum() == 100,
          "sample shading range");
    pipeline->checkbox->setChecked(true);
    fast_gpu->checkbox->setChecked(true);
    dynamic->combobox->setCurrentIndex(3);
    vertex->checkbox->setChecked(true);
    samples->slider->setValue(37);
    bool percentage_visible = false;
    for (const auto* label : samples->findChildren<QLabel*>()) {
        percentage_visible |= label->text() == QStringLiteral("37%");
    }
    Check(percentage_visible, "sample shading percentage feedback");
    for (const auto& function : apply) function(false);
    global.SaveAllValues();
    {
        QSettings saved(QString::fromStdString(global.GetConfigFilePath()), QSettings::IniFormat);
        Check(!saved.value(QStringLiteral("Renderer/use_graphics_pipeline_library/default")).toBool(),
              "explicit pipeline opt-in is not marked as default");
        Check(!saved.value(QStringLiteral("Renderer/use_fast_gpu_time/default")).toBool(),
              "explicit fast GPU opt-in is not marked as default");
    }
    Settings::values.use_graphics_pipeline_library = false;
    Settings::values.use_fast_gpu_time = false;
    Settings::values.dyna_state = Settings::ExtendedDynamicState::Disabled;
    Settings::values.vertex_input_dynamic_state = false;
    Settings::values.sample_shading = 0;
    global.ReloadAllValues();
    Check(Settings::values.use_graphics_pipeline_library.GetValue(), "global pipeline reload");
    Check(Settings::values.use_fast_gpu_time.GetValue(), "explicit fast GPU opt-in survives reload");
    Check(Settings::values.dyna_state.GetValue() == Settings::ExtendedDynamicState::EDS3,
          "global enum reload");
    Check(Settings::values.vertex_input_dynamic_state.GetValue(), "global vertex reload");
    Check(Settings::values.sample_shading.GetValue() == 37, "global scalar reload");

    Settings::SetConfiguringGlobal(false);
    QtConfig game("0100000000001234", Config::ConfigType::PerGameConfig);
    CheckAdvancedCategories();
    std::vector<std::function<void(bool)>> game_apply;
    auto* override_pipeline = builder.BuildWidget(&Settings::values.use_graphics_pipeline_library,
                                                  game_apply);
    auto* override_dynamic = builder.BuildWidget(&Settings::values.dyna_state, game_apply);
    auto* override_vertex = builder.BuildWidget(&Settings::values.vertex_input_dynamic_state,
                                                game_apply);
    auto* override_samples = builder.BuildWidget(&Settings::values.sample_shading, game_apply);
    Check(override_pipeline && override_pipeline->restore_button, "per-game inheritance control");
    override_pipeline->checkbox->click();
    override_dynamic->combobox->setCurrentIndex(1);
    QMetaObject::invokeMethod(override_dynamic->combobox, "activated", Q_ARG(int, 1));
    override_vertex->checkbox->click();
    override_samples->slider->setValue(73);
    QMetaObject::invokeMethod(override_samples->slider, "actionTriggered", Q_ARG(int, 1));
    for (const auto& function : game_apply) function(false);
    game.SaveAllValues();
    Settings::values.use_graphics_pipeline_library.SetGlobal(true);
    game.ReloadAllValues();
    Check(!Settings::values.use_graphics_pipeline_library.UsingGlobal(), "override persisted");
    Check(!Settings::values.use_graphics_pipeline_library.GetValue(), "override value persisted");
    Check(!Settings::values.dyna_state.UsingGlobal() &&
              Settings::values.dyna_state.GetValue() == Settings::ExtendedDynamicState::EDS1,
          "per-game enum persisted");
    Check(!Settings::values.vertex_input_dynamic_state.UsingGlobal() &&
              !Settings::values.vertex_input_dynamic_state.GetValue(), "per-game vertex persisted");
    Check(!Settings::values.sample_shading.UsingGlobal() &&
              Settings::values.sample_shading.GetValue() == 73, "per-game scalar persisted");
    override_pipeline->restore_button->click();
    override_dynamic->restore_button->click();
    override_vertex->restore_button->click();
    override_samples->restore_button->click();
    for (const auto& function : game_apply) function(false);
    game.SaveAllValues();
    Settings::values.use_graphics_pipeline_library.SetGlobal(false);
    game.ReloadAllValues();
    Check(Settings::values.use_graphics_pipeline_library.UsingGlobal(), "inheritance persisted");
    Check(Settings::values.use_graphics_pipeline_library.GetValue(), "global value inherited");
    Check(Settings::values.dyna_state.UsingGlobal() &&
              Settings::values.dyna_state.GetValue() == Settings::ExtendedDynamicState::EDS3,
          "global enum inherited");
    Check(Settings::values.vertex_input_dynamic_state.UsingGlobal() &&
              Settings::values.vertex_input_dynamic_state.GetValue(), "global vertex inherited");
    Check(Settings::values.sample_shading.UsingGlobal() &&
              Settings::values.sample_shading.GetValue() == 37, "global scalar inherited");
    std::cout << "PASS: actual Qt widgets and global/per-game INI round trips\n";
}
