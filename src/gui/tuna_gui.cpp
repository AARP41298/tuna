/*************************************************************************
 * This file is part of tuna
 * github.com/univrsal/tuna
 * Copyright 2021 univrsal <uni@vrsal.de>.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *************************************************************************/

#include "tuna_gui.hpp"
#include "../query/vlc_obs_source.hpp"
#include "../util/config.hpp"
#include "../util/constants.hpp"
#include "../util/tuna_thread.hpp"
#include "../util/utility.hpp"
#include "music_control.hpp"
#include "output_edit_dialog.hpp"
#include "ui_tuna_gui.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QString>
#include <curl/curl.h>
#include <mongoose.h>
#include <mpd/client.h>
#include <obs-frontend-api.h>
#include <string>
#include <taglib/taglib.h>
#include <util/platform.h>

tuna_gui* tuna_dialog = nullptr;

tuna_gui::tuna_gui(QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::tuna_gui)
{
    ui->setupUi(this);
    connect(ui->buttonBox->button(QDialogButtonBox::Apply), SIGNAL(clicked()), this, SLOT(apply_pressed()));
    connect(ui->buttonBox->button(QDialogButtonBox::Ok), SIGNAL(clicked()), this, SLOT(tuna_gui_accepted()));
    connect(this, &tuna_gui::source_registered, this, &tuna_gui::add_music_source);

    /* Other signals */
#define ADD_SIGNAL(btn) connect(ui->btn, SIGNAL(clicked()), this, SLOT(btn##_clicked()))

    ADD_SIGNAL(btn_browse_song_cover);
    ADD_SIGNAL(btn_add_output);
    ADD_SIGNAL(btn_remove_output);
    ADD_SIGNAL(btn_edit_output);
    ADD_SIGNAL(btn_start);
    ADD_SIGNAL(btn_stop);
    ADD_SIGNAL(btn_browse_song_lyrics);

#undef ADD_SIGNAL

    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    ui->tbl_outputs->setColumnWidth(0, 100);
    ui->tbl_outputs->setColumnWidth(1, 180);
    /* TODO Lyrics */
    ui->frame_lyrics->setVisible(false);

    auto about_text = ui->label->text();
#define MAKE_VERSION_STRING(prefix, postfix)                                                       \
    QString("%1.%2.%3")                                                                            \
        .arg(QString::number(prefix##_MAJOR_##postfix), QString::number(prefix##_MINOR_##postfix), \
            QString::number(prefix##_PATCH_##postfix))
    auto* ver = curl_version_info(CURLVERSION_NOW);

    if (ver)
        about_text = about_text.replace("%curlversion%", ver->version);
    about_text = about_text.replace("%qtversion%", QT_VERSION_STR);
    about_text = about_text.replace("%libobsversion%", MAKE_VERSION_STRING(LIBOBS_API, VER));
    about_text = about_text.replace("%taglibversion%", MAKE_VERSION_STRING(TAGLIB, VERSION));
    about_text = about_text.replace("%mpdversion%", MAKE_VERSION_STRING(LIBMPDCLIENT, VERSION));
    about_text = about_text.replace("%mgversion%", MG_VERSION);

    ui->label->setText(about_text);

    m_refresh = new QTimer(this);
    connect(m_refresh, &QTimer::timeout, this, &tuna_gui::refresh);
    m_refresh->start(250);
}

void tuna_gui::choose_file(QString& path, const char* title, const char* file_types)
{
    path = QFileDialog::getSaveFileName(this, tr(title), QDir::home().path(), tr(file_types));
}

void tuna_gui::set_state()
{
    if (tuna_thread::thread_flag)
        ui->lbl_status->setText(T_STATUS_RUNNING);
    else
        ui->lbl_status->setText(T_STATUS_STOPPED);
}

tuna_gui::~tuna_gui()
{
    delete ui;
}

void tuna_gui::toggleShowHide()
{
    setVisible(!isVisible());
    if (isVisible()) {
        /* Load config values for sources on dialog show */
        music_sources::set_gui_values();

        /* load basic values */
        ui->txt_song_cover->setText(utf8_to_qt(config::cover_path));
        ui->txt_song_lyrics->setText(utf8_to_qt(config::lyrics_path));
        ui->sb_refresh_rate->setValue(config::refresh_rate);
        ui->txt_song_placeholder->setText(utf8_to_qt(config::placeholder));
        ui->cb_dl_cover->setChecked(config::download_cover);
        ui->cb_source->setCurrentIndex(ui->cb_source->findData(utf8_to_qt(config::selected_source)));
        ui->cb_host_server->setChecked(CGET_BOOL(CFG_SERVER_ENABLED));
        ui->sb_web_port->setValue(utf8_to_qt(CGET_STR(CFG_SERVER_PORT)).toInt());
        ui->cb_remove_file_extensions->setChecked(config::remove_file_extensions);
        set_state();

        const auto s = CGET_STR(CFG_SELECTED_SOURCE);
        auto i = 0;
        for (const auto& src : qAsConst(music_sources::instances)) {
            if (strcmp(src->id(), s) == 0)
                break;
            i++;
        }
        ui->cb_source->setCurrentIndex(i);

        /* Load table contents */
        int row = 1; /* Clear all rows except headers */
        for (; row < ui->tbl_outputs->rowCount(); row++)
            ui->tbl_outputs->removeRow(row);
        row = 0; /* Load rows */
        ui->tbl_outputs->setRowCount(config::outputs.size());
        for (const auto& entry : qAsConst(config::outputs)) {
            ui->tbl_outputs->setItem(row, 0, new QTableWidgetItem(entry.log_mode ? "Yes" : "No"));
            ui->tbl_outputs->setItem(row, 1, new QTableWidgetItem(entry.format));
            ui->tbl_outputs->setItem(row, 2, new QTableWidgetItem(entry.path));
            row++;
        }
    }
}

void tuna_gui::add_music_source(const QString& display, const QString& id, source_widget* w)
{
    ui->cb_source->addItem(display, id);
    if (w) {
        ui->settings_tabs->insertTab(1, w, display);
        m_source_widgets.append(w);
    }
}

void tuna_gui::refresh()
{
    for (auto widget : qAsConst(m_source_widgets))
        widget->tick();
}

void tuna_gui::tuna_gui_accepted()
{
    QString tmp = ui->cb_source->currentData().toByteArray();
    CSET_STR(CFG_SELECTED_SOURCE, tmp.toStdString().c_str());
    CSET_STR(CFG_COVER_PATH, qt_to_utf8(ui->txt_song_cover->text()));
    CSET_STR(CFG_LYRICS_PATH, qt_to_utf8(ui->txt_song_lyrics->text()));
    CSET_UINT(CFG_REFRESH_RATE, ui->sb_refresh_rate->value());
    CSET_STR(CFG_SONG_PLACEHOLDER, qt_to_utf8(ui->txt_song_placeholder->text()));
    CSET_BOOL(CFG_DOWNLOAD_COVER, ui->cb_dl_cover->isChecked());
    CSET_BOOL(CFG_SERVER_ENABLED, ui->cb_host_server->isChecked());
    CSET_STR(CFG_SERVER_PORT, qt_to_utf8(QString::number(ui->sb_web_port->value())));
    CSET_BOOL(CFG_REMOVE_EXTENSIONS, ui->cb_remove_file_extensions->isChecked());

    /* save outputs */
    tuna_thread::thread_mutex.lock();
    config::outputs.clear();
    for (int row = 0; row < ui->tbl_outputs->rowCount(); row++) {
        config::output tmp;
        tmp.log_mode = ui->tbl_outputs->item(row, 0)->text() == "Yes";
        tmp.format = ui->tbl_outputs->item(row, 1)->text();
        tmp.path = ui->tbl_outputs->item(row, 2)->text();
        config::outputs.push_back(tmp);
    }
    config::save_outputs();

    for (auto& w : m_source_widgets) {
        if (w)
            w->save_settings();
    }

    config::refresh_rate = ui->sb_refresh_rate->value();
    tuna_thread::thread_mutex.unlock();
    config::load();
}

void tuna_gui::apply_pressed()
{
    tuna_gui_accepted();
}

void tuna_gui::btn_start_clicked()
{
    if (!tuna_thread::start())
        QMessageBox::warning(this, "Error", "Thread couldn't be started!");
    CSET_BOOL(CFG_RUNNING, tuna_thread::thread_flag);
    set_state();
}

void tuna_gui::btn_stop_clicked()
{
    tuna_thread::stop();
    CSET_BOOL(CFG_RUNNING, tuna_thread::thread_flag);
    set_state();
}

void tuna_gui::btn_browse_song_cover_clicked()
{
    QString path;
    choose_file(path, T_SELECT_COVER_FILE, FILTER("Image file", "*.png"));
    if (!path.isEmpty())
        ui->txt_song_cover->setText(path);
}

void tuna_gui::btn_browse_song_lyrics_clicked()
{
    QString path;
    choose_file(path, T_SELECT_LYRICS_FILE, FILTER("Text file", "*.txt"));
    if (!path.isEmpty())
        ui->txt_song_lyrics->setText(path);
}

void tuna_gui::add_output(const QString& format, const QString& path, bool log_mode)
{
    int row = ui->tbl_outputs->rowCount();
    ui->tbl_outputs->insertRow(row);
    ui->tbl_outputs->setItem(row, 0, new QTableWidgetItem(log_mode ? "Yes" : "No"));
    ui->tbl_outputs->setItem(row, 1, new QTableWidgetItem(format));
    ui->tbl_outputs->setItem(row, 2, new QTableWidgetItem(path));
}

void tuna_gui::edit_output(const QString& format, const QString& path, bool log_mode)
{
    auto selection = ui->tbl_outputs->selectedItems();
    if (!selection.empty() && selection.size() > 1) {
        selection.at(0)->setText(log_mode ? "Yes" : "No");
        selection.at(1)->setText(format);
        selection.at(2)->setText(path);
    }
}

void tuna_gui::btn_add_output_clicked()
{
    obs_frontend_push_ui_translation(obs_module_get_string);
    auto* dialog = new output_edit_dialog(edit_mode::create, this);
    obs_frontend_pop_ui_translation();
    dialog->exec();
}

void tuna_gui::btn_remove_output_clicked()
{
    auto* select = ui->tbl_outputs->selectionModel();
    if (select->hasSelection()) {
        auto rows = select->selectedRows();
        if (!rows.empty()) {
            ui->tbl_outputs->removeRow(rows.first().row());
        }
    }
}

void tuna_gui::get_selected_output(QString& format, QString& path, bool& log_mode)
{
    auto selection = ui->tbl_outputs->selectedItems();
    if (!selection.empty() && selection.size() > 1) {
        log_mode = selection.at(0)->text() == "Yes";
        format = selection.at(1)->text();
        path = selection.at(2)->text();
    }
}

void tuna_gui::btn_edit_output_clicked()
{
    auto selection = ui->tbl_outputs->selectedItems();
    if (!selection.empty() && selection.size() > 1) {
        obs_frontend_push_ui_translation(obs_module_get_string);
        auto* dialog = new output_edit_dialog(edit_mode::modify, this);
        obs_frontend_pop_ui_translation();
        dialog->exec();
    }
}
