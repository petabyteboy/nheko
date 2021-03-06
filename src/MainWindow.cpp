/*
 * nheko Copyright (C) 2017  Konstantinos Sideris <siderisk@auth.gr>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <QApplication>
#include <QLayout>
#include <QSettings>
#include <QShortcut>

#include <mtx/requests.hpp>

#include "ChatPage.h"
#include "Config.h"
#include "Logging.h"
#include "LoginPage.h"
#include "MainWindow.h"
#include "MatrixClient.h"
#include "RegisterPage.h"
#include "TrayIcon.h"
#include "UserSettingsPage.h"
#include "WelcomePage.h"
#include "ui/LoadingIndicator.h"
#include "ui/OverlayModal.h"
#include "ui/SnackBar.h"

#include "dialogs/CreateRoom.h"
#include "dialogs/InviteUsers.h"
#include "dialogs/JoinRoom.h"
#include "dialogs/LeaveRoom.h"
#include "dialogs/Logout.h"
#include "dialogs/MemberList.h"
#include "dialogs/RoomSettings.h"

MainWindow *MainWindow::instance_ = nullptr;

MainWindow::MainWindow(QWidget *parent)
  : QMainWindow(parent)
  , progressModal_{nullptr}
  , spinner_{nullptr}
{
        setWindowTitle("nheko");
        setObjectName("MainWindow");

        restoreWindowSize();

        QFont font("Open Sans");
        font.setPixelSize(conf::fontSize);
        font.setStyleStrategy(QFont::PreferAntialias);
        setFont(font);

        userSettings_ = QSharedPointer<UserSettings>(new UserSettings);
        trayIcon_     = new TrayIcon(":/logos/nheko-32.png", this);

        welcome_page_     = new WelcomePage(this);
        login_page_       = new LoginPage(this);
        register_page_    = new RegisterPage(this);
        chat_page_        = new ChatPage(userSettings_, this);
        userSettingsPage_ = new UserSettingsPage(userSettings_, this);

        // Initialize sliding widget manager.
        pageStack_ = new QStackedWidget(this);
        pageStack_->addWidget(welcome_page_);
        pageStack_->addWidget(login_page_);
        pageStack_->addWidget(register_page_);
        pageStack_->addWidget(chat_page_);
        pageStack_->addWidget(userSettingsPage_);

        setCentralWidget(pageStack_);

        connect(welcome_page_, SIGNAL(userLogin()), this, SLOT(showLoginPage()));
        connect(welcome_page_, SIGNAL(userRegister()), this, SLOT(showRegisterPage()));

        connect(login_page_, SIGNAL(backButtonClicked()), this, SLOT(showWelcomePage()));
        connect(login_page_, &LoginPage::loggingIn, this, &MainWindow::showOverlayProgressBar);
        connect(
          register_page_, &RegisterPage::registering, this, &MainWindow::showOverlayProgressBar);
        connect(
          login_page_, &LoginPage::errorOccurred, this, [this]() { removeOverlayProgressBar(); });
        connect(register_page_, &RegisterPage::errorOccurred, this, [this]() {
                removeOverlayProgressBar();
        });
        connect(register_page_, SIGNAL(backButtonClicked()), this, SLOT(showWelcomePage()));

        connect(chat_page_, &ChatPage::closing, this, &MainWindow::showWelcomePage);
        connect(
          chat_page_, &ChatPage::showOverlayProgressBar, this, &MainWindow::showOverlayProgressBar);
        connect(
          chat_page_, SIGNAL(changeWindowTitle(QString)), this, SLOT(setWindowTitle(QString)));
        connect(chat_page_, SIGNAL(unreadMessages(int)), trayIcon_, SLOT(setUnreadCount(int)));
        connect(chat_page_, &ChatPage::showLoginPage, this, [this](const QString &msg) {
                login_page_->loginError(msg);
                showLoginPage();
        });

        connect(userSettingsPage_, &UserSettingsPage::moveBack, this, [this]() {
                pageStack_->setCurrentWidget(chat_page_);
        });

        connect(
          userSettingsPage_, SIGNAL(trayOptionChanged(bool)), trayIcon_, SLOT(setVisible(bool)));

        connect(trayIcon_,
                SIGNAL(activated(QSystemTrayIcon::ActivationReason)),
                this,
                SLOT(iconActivated(QSystemTrayIcon::ActivationReason)));

        connect(chat_page_, SIGNAL(contentLoaded()), this, SLOT(removeOverlayProgressBar()));
        connect(
          chat_page_, &ChatPage::showUserSettingsPage, this, &MainWindow::showUserSettingsPage);

        connect(login_page_, &LoginPage::loginOk, this, [this](const mtx::responses::Login &res) {
                http::client()->set_user(res.user_id);
                showChatPage();
        });

        connect(register_page_, &RegisterPage::registerOk, this, &MainWindow::showChatPage);

        QShortcut *quitShortcut = new QShortcut(QKeySequence::Quit, this);
        connect(quitShortcut, &QShortcut::activated, this, QApplication::quit);

        QShortcut *quickSwitchShortcut = new QShortcut(QKeySequence("Ctrl+K"), this);
        connect(quickSwitchShortcut, &QShortcut::activated, this, [this]() {
                if (chat_page_->isVisible() && !hasActiveDialogs())
                        chat_page_->showQuickSwitcher();
        });

        QSettings settings;

        trayIcon_->setVisible(userSettings_->isTrayEnabled());

        if (hasActiveUser()) {
                QString token       = settings.value("auth/access_token").toString();
                QString home_server = settings.value("auth/home_server").toString();
                QString user_id     = settings.value("auth/user_id").toString();
                QString device_id   = settings.value("auth/device_id").toString();

                http::client()->set_access_token(token.toStdString());
                http::client()->set_server(home_server.toStdString());
                http::client()->set_device_id(device_id.toStdString());

                try {
                        using namespace mtx::identifiers;
                        http::client()->set_user(parse<User>(user_id.toStdString()));
                } catch (const std::invalid_argument &e) {
                        nhlog::ui()->critical("bootstrapped with invalid user_id: {}",
                                              user_id.toStdString());
                }

                showChatPage();
        }
}

void
MainWindow::showEvent(QShowEvent *event)
{
        adjustSideBars();
        QMainWindow::showEvent(event);
}

void
MainWindow::resizeEvent(QResizeEvent *event)
{
        adjustSideBars();
        QMainWindow::resizeEvent(event);
}

void
MainWindow::adjustSideBars()
{
        const int timelineWidth = chat_page_->timelineWidth();
        const int minAvailableWidth =
          conf::sideBarCollapsePoint + ui::sidebar::CommunitiesSidebarSize;

        if (timelineWidth < minAvailableWidth && !chat_page_->isSideBarExpanded()) {
                chat_page_->hideSideBars();
        } else {
                chat_page_->showSideBars();
        }
}

void
MainWindow::restoreWindowSize()
{
        QSettings settings;
        int savedWidth  = settings.value("window/width").toInt();
        int savedheight = settings.value("window/height").toInt();

        if (savedWidth == 0 || savedheight == 0)
                resize(conf::window::width, conf::window::height);
        else
                resize(savedWidth, savedheight);
}

void
MainWindow::saveCurrentWindowSize()
{
        QSettings settings;
        QSize current = size();

        settings.setValue("window/width", current.width());
        settings.setValue("window/height", current.height());
}

void
MainWindow::removeOverlayProgressBar()
{
        QTimer *timer = new QTimer(this);
        timer->setSingleShot(true);

        connect(timer, &QTimer::timeout, [this, timer]() {
                timer->deleteLater();

                if (!progressModal_.isNull())
                        progressModal_->hide();

                if (!spinner_.isNull())
                        spinner_->stop();

                progressModal_.reset();
                spinner_.reset();
        });

        // FIXME:  Snackbar doesn't work if it's initialized in the constructor.
        QTimer::singleShot(0, this, [this]() {
                snackBar_ = QSharedPointer<SnackBar>(new SnackBar(this));
                connect(chat_page_,
                        &ChatPage::showNotification,
                        snackBar_.data(),
                        &SnackBar::showMessage);
        });

        timer->start(500);
}

void
MainWindow::showChatPage()
{
        auto userid     = QString::fromStdString(http::client()->user_id().to_string());
        auto device_id  = QString::fromStdString(http::client()->device_id());
        auto homeserver = QString::fromStdString(http::client()->server() + ":" +
                                                 std::to_string(http::client()->port()));
        auto token      = QString::fromStdString(http::client()->access_token());

        QSettings settings;
        settings.setValue("auth/access_token", token);
        settings.setValue("auth/home_server", homeserver);
        settings.setValue("auth/user_id", userid);
        settings.setValue("auth/device_id", device_id);

        showOverlayProgressBar();

        pageStack_->setCurrentWidget(chat_page_);

        pageStack_->removeWidget(welcome_page_);
        pageStack_->removeWidget(login_page_);
        pageStack_->removeWidget(register_page_);

        login_page_->reset();
        chat_page_->bootstrap(userid, homeserver, token);

        instance_ = this;
}

void
MainWindow::closeEvent(QCloseEvent *event)
{
        if (!qApp->isSavingSession() && isVisible() && pageSupportsTray() &&
            userSettings_->isTrayEnabled()) {
                event->ignore();
                hide();
        }
}

void
MainWindow::iconActivated(QSystemTrayIcon::ActivationReason reason)
{
        switch (reason) {
        case QSystemTrayIcon::Trigger:
                if (!isVisible()) {
                        show();
                } else {
                        hide();
                }
                break;
        default:
                break;
        }
}

bool
MainWindow::hasActiveUser()
{
        QSettings settings;

        return settings.contains("auth/access_token") && settings.contains("auth/home_server") &&
               settings.contains("auth/user_id");
}

void
MainWindow::openUserProfile(const QString &user_id, const QString &room_id)
{
        if (!userProfileDialog_)
                userProfileDialog_ =
                  QSharedPointer<dialogs::UserProfile>(new dialogs::UserProfile(this));

        userProfileDialog_->init(user_id, room_id);

        if (!userProfileModal_)
                userProfileModal_ =
                  QSharedPointer<OverlayModal>(new OverlayModal(this, userProfileDialog_.data()));

        userProfileModal_->setContentAlignment(Qt::AlignTop | Qt::AlignHCenter);
        userProfileModal_->show();
}

void
MainWindow::openRoomSettings(const QString &room_id)
{
        const auto roomToSearch = room_id.isEmpty() ? chat_page_->currentRoom() : "";

        roomSettingsDialog_ =
          QSharedPointer<dialogs::RoomSettings>(new dialogs::RoomSettings(roomToSearch, this));

        connect(roomSettingsDialog_.data(), &dialogs::RoomSettings::closing, this, [this]() {
                roomSettingsModal_->hide();
        });

        roomSettingsModal_ =
          QSharedPointer<OverlayModal>(new OverlayModal(this, roomSettingsDialog_.data()));

        roomSettingsModal_->show();
}

void
MainWindow::openMemberListDialog(const QString &room_id)
{
        const auto roomToSearch = room_id.isEmpty() ? chat_page_->currentRoom() : "";

        memberListDialog_ =
          QSharedPointer<dialogs::MemberList>(new dialogs::MemberList(roomToSearch, this));

        memberListModal_ =
          QSharedPointer<OverlayModal>(new OverlayModal(this, memberListDialog_.data()));

        memberListModal_->show();
}

void
MainWindow::openLeaveRoomDialog(const QString &room_id)
{
        auto roomToLeave = room_id.isEmpty() ? chat_page_->currentRoom() : room_id;

        leaveRoomDialog_ = QSharedPointer<dialogs::LeaveRoom>(new dialogs::LeaveRoom(this));

        connect(leaveRoomDialog_.data(),
                &dialogs::LeaveRoom::closing,
                this,
                [this, roomToLeave](bool leaving) {
                        leaveRoomModal_->hide();

                        if (leaving)
                                chat_page_->leaveRoom(roomToLeave);
                });

        leaveRoomModal_ =
          QSharedPointer<OverlayModal>(new OverlayModal(this, leaveRoomDialog_.data()));
        leaveRoomModal_->setColor(QColor(30, 30, 30, 170));

        leaveRoomModal_->show();
}

void
MainWindow::showOverlayProgressBar()
{
        if (spinner_.isNull()) {
                spinner_ = QSharedPointer<LoadingIndicator>(
                  new LoadingIndicator(this),
                  [](LoadingIndicator *indicator) { indicator->deleteLater(); });
                spinner_->setFixedHeight(100);
                spinner_->setFixedWidth(100);
                spinner_->setObjectName("ChatPageLoadSpinner");
                spinner_->start();
        }

        if (progressModal_.isNull()) {
                progressModal_ =
                  QSharedPointer<OverlayModal>(new OverlayModal(this, spinner_.data()),
                                               [](OverlayModal *modal) { modal->deleteLater(); });
                progressModal_->setColor(QColor(30, 30, 30));
                progressModal_->setDismissible(false);
                progressModal_->show();
        }
}

void
MainWindow::openInviteUsersDialog(std::function<void(const QStringList &invitees)> callback)
{
        if (inviteUsersDialog_.isNull()) {
                inviteUsersDialog_ =
                  QSharedPointer<dialogs::InviteUsers>(new dialogs::InviteUsers(this));

                connect(inviteUsersDialog_.data(),
                        &dialogs::InviteUsers::closing,
                        this,
                        [this, callback](bool isSending, QStringList invitees) {
                                inviteUsersModal_->hide();

                                if (isSending && !invitees.isEmpty())
                                        callback(invitees);
                        });
        }

        if (inviteUsersModal_.isNull()) {
                inviteUsersModal_ = QSharedPointer<OverlayModal>(
                  new OverlayModal(MainWindow::instance(), inviteUsersDialog_.data()));
                inviteUsersModal_->setColor(QColor(30, 30, 30, 170));
        }

        inviteUsersModal_->show();
}

void
MainWindow::openJoinRoomDialog(std::function<void(const QString &room_id)> callback)
{
        if (joinRoomDialog_.isNull()) {
                joinRoomDialog_ = QSharedPointer<dialogs::JoinRoom>(new dialogs::JoinRoom(this));

                connect(joinRoomDialog_.data(),
                        &dialogs::JoinRoom::closing,
                        this,
                        [this, callback](bool isJoining, const QString &room) {
                                joinRoomModal_->hide();

                                if (isJoining && !room.isEmpty())
                                        callback(room);
                        });
        }

        if (joinRoomModal_.isNull()) {
                joinRoomModal_ = QSharedPointer<OverlayModal>(
                  new OverlayModal(MainWindow::instance(), joinRoomDialog_.data()));
        }

        joinRoomModal_->show();
}

void
MainWindow::openCreateRoomDialog(
  std::function<void(const mtx::requests::CreateRoom &request)> callback)
{
        if (createRoomDialog_.isNull()) {
                createRoomDialog_ =
                  QSharedPointer<dialogs::CreateRoom>(new dialogs::CreateRoom(this));

                connect(
                  createRoomDialog_.data(),
                  &dialogs::CreateRoom::closing,
                  this,
                  [this, callback](bool isCreating, const mtx::requests::CreateRoom &request) {
                          createRoomModal_->hide();

                          if (isCreating)
                                  callback(request);
                  });
        }

        if (createRoomModal_.isNull()) {
                createRoomModal_ = QSharedPointer<OverlayModal>(
                  new OverlayModal(MainWindow::instance(), createRoomDialog_.data()));
        }

        createRoomModal_->show();
}

void
MainWindow::openLogoutDialog(std::function<void()> callback)
{
        if (logoutDialog_.isNull()) {
                logoutDialog_ = QSharedPointer<dialogs::Logout>(new dialogs::Logout(this));
                connect(logoutDialog_.data(),
                        &dialogs::Logout::closing,
                        this,
                        [this, callback](bool logging_out) {
                                logoutModal_->hide();

                                if (logging_out)
                                        callback();
                        });
        }

        if (logoutModal_.isNull()) {
                logoutModal_ = QSharedPointer<OverlayModal>(
                  new OverlayModal(MainWindow::instance(), logoutDialog_.data()));
        }

        logoutModal_->show();
}

bool
MainWindow::hasActiveDialogs() const
{
        return (!leaveRoomModal_.isNull() && leaveRoomModal_->isVisible()) ||
               (!progressModal_.isNull() && progressModal_->isVisible()) ||
               (!inviteUsersModal_.isNull() && inviteUsersModal_->isVisible()) ||
               (!joinRoomModal_.isNull() && joinRoomModal_->isVisible()) ||
               (!createRoomModal_.isNull() && createRoomModal_->isVisible()) ||
               (!logoutModal_.isNull() && logoutModal_->isVisible());
}

bool
MainWindow::pageSupportsTray() const
{
        return !welcome_page_->isVisible() && !login_page_->isVisible() &&
               !register_page_->isVisible();
}
