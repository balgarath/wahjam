/*
    Copyright (C) 2012 Stefan Hajnoczi <stefanha@gmail.com>

    Wahjam is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Wahjam is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Wahjam; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <QMessageBox>
#include <QVBoxLayout>
#include <QPushButton>
#include <QSplitter>
#include <QMenuBar>
#include <QMenu>
#include <QStatusBar>
#include <QSettings>
#include <QDateTime>
#include <QDir>
#include <QDesktopServices>
#include <QUrl>

#include "MainWindow.h"
#include "ClientRunThread.h"
#include "ConnectDialog.h"
#include "PortAudioConfigDialog.h"
#include "../../WDL/jnetlib/jnetlib.h"
#include "../njmisc.h"

MainWindow *MainWindow::instance; /* singleton */

void MainWindow::OnSamplesTrampoline(float **inbuf, int innch, float **outbuf, int outnch, int len, int srate)
{
  MainWindow::GetInstance()->OnSamples(inbuf, innch, outbuf, outnch, len, srate);
}

int MainWindow::LicenseCallbackTrampoline(int user32, char *licensetext)
{
  /* Bounce back into ClientRunThread */
  return MainWindow::GetInstance()->runThread->licenseCallbackTrampoline(licensetext);
}

void MainWindow::ChatMessageCallbackTrampoline(int user32, NJClient *inst, char **parms, int nparms)
{
  /* Bounce back into ClientRunThread */
  MainWindow::GetInstance()->runThread->chatMessageCallbackTrampoline(parms, nparms);
}

MainWindow *MainWindow::GetInstance()
{
  return instance;
}

MainWindow::MainWindow(QWidget *parent)
  : QMainWindow(parent), audio(NULL)
{
  /* Since the ninjam callbacks do not pass a void* opaque argument we rely on
   * a global variable.
   */
  if (MainWindow::instance) {
    fprintf(stderr, "MainWindow can only be instantiated once!\n");
    abort();
  }
  MainWindow::instance = this;

  JNL::open_socketlib();

  client.config_savelocalaudio = 0;
  client.LicenseAgreementCallback = LicenseCallbackTrampoline;
  client.ChatMessage_Callback = ChatMessageCallbackTrampoline;
  client.SetLocalChannelInfo(0, "channel0", true, 0, false, 0, true, true);
  client.SetLocalChannelMonitoring(0, false, 0.0f, false, 0.0f, false, false, false, false);

  connectAction = new QAction(tr("&Connect..."), this);
  connect(connectAction, SIGNAL(triggered()), this, SLOT(ShowConnectDialog()));

  disconnectAction = new QAction(tr("&Disconnect"), this);
  disconnectAction->setEnabled(false);
  connect(disconnectAction, SIGNAL(triggered()), this, SLOT(Disconnect()));

  audioConfigAction = new QAction(tr("Configure &audio..."), this);
  connect(audioConfigAction, SIGNAL(triggered()), this, SLOT(ShowAudioConfigDialog()));

  QAction *exitAction = new QAction(tr("E&xit"), this);
  exitAction->setShortcuts(QKeySequence::Quit);
  connect(exitAction, SIGNAL(triggered()), this, SLOT(close()));

  QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
  fileMenu->addAction(connectAction);
  fileMenu->addAction(disconnectAction);
  fileMenu->addAction(audioConfigAction);
  fileMenu->addAction(exitAction);

  QAction *aboutAction = new QAction(tr("&About..."), this);
  connect(aboutAction, SIGNAL(triggered()), this, SLOT(ShowAboutDialog()));

  QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
  helpMenu->addAction(aboutAction);

  setupStatusBar();

  setWindowTitle(tr("Wahjam"));

  chatOutput = new QTextEdit(this);
  chatOutput->setReadOnly(true);

  chatInput = new QLineEdit(this);
  chatInput->connect(chatInput, SIGNAL(returnPressed()),
                     this, SLOT(ChatInputReturnPressed()));

  channelTree = new ChannelTreeWidget(this);
  setupChannelTree();
  connect(channelTree, SIGNAL(MetronomeMuteChanged(bool)),
          this, SLOT(MetronomeMuteChanged(bool)));
  connect(channelTree, SIGNAL(MetronomeBoostChanged(bool)),
          this, SLOT(MetronomeBoostChanged(bool)));
  connect(channelTree, SIGNAL(LocalChannelMuteChanged(int, bool)),
          this, SLOT(LocalChannelMuteChanged(int, bool)));
  connect(channelTree, SIGNAL(LocalChannelBoostChanged(int, bool)),
          this, SLOT(LocalChannelBoostChanged(int, bool)));
  connect(channelTree, SIGNAL(LocalChannelBroadcastChanged(int, bool)),
          this, SLOT(LocalChannelBroadcastChanged(int, bool)));
  connect(channelTree, SIGNAL(RemoteChannelMuteChanged(int, int, bool)),
          this, SLOT(RemoteChannelMuteChanged(int, int, bool)));

  metronomeBar = new MetronomeBar(this);
  connect(this, SIGNAL(Disconnected()),
          metronomeBar, SLOT(reset()));

  QSplitter *splitter = new QSplitter(this);
  QWidget *content = new QWidget;
  QVBoxLayout *layout = new QVBoxLayout;

  layout->addWidget(chatOutput);
  layout->addWidget(chatInput);
  layout->addWidget(metronomeBar);
  content->setLayout(layout);
  content->setTabOrder(chatInput, chatOutput);

  splitter->addWidget(channelTree);
  splitter->addWidget(content);
  splitter->setOrientation(Qt::Vertical);

  setCentralWidget(splitter);

  BeatsPerIntervalChanged(0);
  BeatsPerMinuteChanged(0);

  runThread = new ClientRunThread(&clientMutex, &client);

  /* Hook up an inter-thread signal for the license agreement dialog */
  connect(runThread, SIGNAL(licenseCallback(const char *, bool *)),
          this, SLOT(LicenseCallback(const char *, bool *)),
          Qt::BlockingQueuedConnection);

  /* Hook up an inter-thread signal for the chat message callback */
  connect(runThread, SIGNAL(chatMessageCallback(char **, int)),
          this, SLOT(ChatMessageCallback(char **, int)),
          Qt::BlockingQueuedConnection);

  /* No need to block for the remote user info callback */
  connect(runThread, SIGNAL(userInfoChanged()),
          this, SLOT(UserInfoChanged()));

  /* Hook up an inter-thread signal for client status changes */
  connect(runThread, SIGNAL(statusChanged(int)),
          this, SLOT(ClientStatusChanged(int)));

  /* Hook up inter-thread signals for bpm/bpi changes */
  connect(runThread, SIGNAL(beatsPerMinuteChanged(int)),
          this, SLOT(BeatsPerMinuteChanged(int)));
  connect(runThread, SIGNAL(beatsPerIntervalChanged(int)),
          this, SLOT(BeatsPerIntervalChanged(int)));

  /* Hook up inter-thread signals for beat and interval changes */
  connect(runThread, SIGNAL(beatsPerIntervalChanged(int)),
          metronomeBar, SLOT(setBeatsPerInterval(int)));
  connect(runThread, SIGNAL(currentBeatChanged(int)),
          metronomeBar, SLOT(setCurrentBeat(int)));

  runThread->start();
}

MainWindow::~MainWindow()
{
  Disconnect();

  if (runThread) {
    runThread->stop();
    delete runThread;
    runThread = NULL;
  }
  JNL::close_socketlib();
}

/* Must be called with client mutex held or before client thread is started */
void MainWindow::setupChannelTree()
{
  int i, ch;
  for (i = 0; (ch = client.EnumLocalChannels(i)) != -1; i++) {
    bool broadcast, mute;
    const char *name = client.GetLocalChannelInfo(ch, NULL, NULL, &broadcast);
    client.GetLocalChannelMonitoring(ch, NULL, NULL, &mute, NULL);

    channelTree->addLocalChannel(ch, QString::fromUtf8(name), mute, broadcast);
  }
}

void MainWindow::setupStatusBar()
{
  bpmLabel = new QLabel(this);
  bpmLabel->setFrameStyle(QFrame::Panel | QFrame::Sunken);
  statusBar()->addPermanentWidget(bpmLabel);

  bpiLabel = new QLabel(this);
  bpiLabel->setFrameStyle(QFrame::Panel | QFrame::Sunken);
  statusBar()->addPermanentWidget(bpiLabel);
}

void MainWindow::Connect(const QString &host, const QString &user, const QString &pass)
{
  if (!setupWorkDir()) {
    chatAddLine("Unable to create work directory.", "");
    return;
  }

  QSettings settings;
  QString hostAPI = settings.value("audio/hostAPI").toString();
  QString inputDevice = settings.value("audio/inputDevice").toString();
  QString outputDevice = settings.value("audio/outputDevice").toString();
  audio = create_audioStreamer_PortAudio(hostAPI.toLocal8Bit().data(),
                                         inputDevice.toLocal8Bit().data(),
                                         outputDevice.toLocal8Bit().data(),
                                         OnSamplesTrampoline);
  if (!audio)
  {
    printf("Error opening audio!\n");
    exit(1);
  }

  audioConfigAction->setEnabled(false);
  connectAction->setEnabled(false);
  disconnectAction->setEnabled(true);

  setWindowTitle(tr("Wahjam - %1").arg(host));

  client.Connect(host.toAscii().data(),
                 user.toUtf8().data(),
                 pass.toUtf8().data());
}

void MainWindow::Disconnect()
{
  delete audio;
  audio = NULL;

  clientMutex.lock();
  client.Disconnect();
  QString workDirPath = QString::fromUtf8(client.GetWorkDir());
  bool keepWorkDir = client.config_savelocalaudio;
  client.SetWorkDir(NULL);
  clientMutex.unlock();

  if (!workDirPath.isEmpty() && !keepWorkDir) {
    cleanupWorkDir(workDirPath);
    chatAddLine("Disconnected", "");
  }

  setWindowTitle(tr("Wahjam"));

  audioConfigAction->setEnabled(true);
  connectAction->setEnabled(true);
  disconnectAction->setEnabled(false);
  BeatsPerMinuteChanged(0);
  BeatsPerIntervalChanged(0);
  emit Disconnected();
}

bool MainWindow::setupWorkDir()
{
  QDir basedir(QDesktopServices::storageLocation(QDesktopServices::DataLocation));

  /* The app data directory might not exist, so create it */
  if (!basedir.mkpath(basedir.absolutePath())) {
    return false;
  }

  /* Filename generation uses date/time plus a unique number, if necessary */
  int i;
  for (i = 0; i < 16; i++) {
    QString filename(QDateTime::currentDateTime().toString("yyyyMMdd_hhmm"));
    if (i > 0) {
      filename += QString("_%1").arg(i);
    }
    filename += ".wahjam";

    if (basedir.mkdir(filename)) {
      client.SetWorkDir(basedir.filePath(filename).toUtf8().data());
      return true;
    }
  }
  return false;
}

void MainWindow::cleanupWorkDir(const QString &path)
{
  QDir workDir(path);

  foreach (const QFileInfo &subdirInfo,
           workDir.entryInfoList(QDir::NoDotAndDotDot | QDir::Dirs)) {
    QDir subdir(subdirInfo.absoluteDir());

    foreach (const QString &file,
             subdir.entryList(QDir::NoDotAndDotDot | QDir::Files)) {
      subdir.remove(file);
    }
    workDir.rmdir(subdirInfo.fileName());
  }

  QString name(workDir.dirName());
  workDir.cdUp();
  workDir.rmdir(name);
}

void MainWindow::ShowConnectDialog()
{
  const QUrl url("http://autosong.ninjam.com/serverlist.php");
  ConnectDialog connectDialog;
  QSettings settings;
  QStringList hosts = settings.value("connect/hosts").toStringList();

  connectDialog.resize(600, 500);
  connectDialog.loadServerList(url);
  connectDialog.setRecentHostsList(hosts);
  connectDialog.setUser(settings.value("connect/user").toString());
  connectDialog.setIsPublicServer(settings.value("connect/public", true).toBool());

  if (connectDialog.exec() != QDialog::Accepted) {
    return;
  }

  hosts.prepend(connectDialog.host());
  hosts.removeDuplicates();
  hosts = hosts.mid(0, 16); /* limit maximum number of elements */

  settings.setValue("connect/hosts", hosts);
  settings.setValue("connect/user", connectDialog.user());
  settings.setValue("connect/public", connectDialog.isPublicServer());

  QString user = connectDialog.user();
  if (connectDialog.isPublicServer()) {
    user.prepend("anonymous:");
  }

  Connect(connectDialog.host(), user, connectDialog.pass());
}

void MainWindow::ShowAudioConfigDialog()
{
  PortAudioConfigDialog audioDialog;
  QSettings settings;

  audioDialog.setHostAPI(settings.value("audio/hostAPI").toString());
  audioDialog.setInputDevice(settings.value("audio/inputDevice").toString());
  audioDialog.setOutputDevice(settings.value("audio/outputDevice").toString());

  if (audioDialog.exec() == QDialog::Accepted) {
    settings.setValue("audio/hostAPI", audioDialog.hostAPI());
    settings.setValue("audio/inputDevice", audioDialog.inputDevice());
    settings.setValue("audio/outputDevice", audioDialog.outputDevice());
  }
}

void MainWindow::ShowAboutDialog()
{
  QMessageBox::about(this, tr("About Wahjam"),
      tr("<h1>Wahjam version %1</h1>"
         "<p><b>Website:</b> <a href=\"http://wahjam.org/\">http://wahjam.org/</a></p>"
         "<p><b>Git commit:</b> <a href=\"http://github.com/wahjam/wahjam/commit/%2\">%2</a></p>"
         "<p>Based on <a href=\"http://ninjam.com/\">NINJAM</a>.</p>"
         "<p>Licensed under the GNU General Public License version 2, see "
         "<a href=\"http://www.gnu.org/licenses/gpl-2.0.html\">"
         "http://www.gnu.org/licenses/gpl-2.0.html</a> for details.</p>").arg(VERSION, COMMIT_ID));
}

void MainWindow::UserInfoChanged()
{
  ChannelTreeWidget::RemoteChannelUpdater updater(channelTree);
  clientMutex.lock();

  int useridx;
  for (useridx = 0; useridx < client.GetNumUsers(); useridx++) {
    const char *name = client.GetUserState(useridx, NULL, NULL, NULL);
    updater.addUser(useridx, QString::fromUtf8(name));

    int channelidx;
    for (channelidx = 0; client.EnumUserChannels(useridx, channelidx) != -1; channelidx++) {
      bool mute;
      name = client.GetUserChannelState(useridx, channelidx, NULL, NULL, NULL, &mute, NULL);
      updater.addChannel(channelidx, QString::fromUtf8(name), mute);
    }
  }

  clientMutex.unlock();
  updater.commit();
}

void MainWindow::OnSamples(float **inbuf, int innch, float **outbuf, int outnch, int len, int srate)
{
  client.AudioProc(inbuf, innch, outbuf, outnch, len, srate);
}

void MainWindow::LicenseCallback(const char *licensetext, bool *result)
{
  QMessageBox msgBox(this);

  msgBox.setText("Please review this server license agreement.");
  msgBox.setInformativeText(licensetext);
  msgBox.setStandardButtons(QMessageBox::Cancel | QMessageBox::Ok);
  msgBox.setTextFormat(Qt::PlainText);

  *result = msgBox.exec() == QMessageBox::Ok ? TRUE : FALSE;
}

void MainWindow::ClientStatusChanged(int newStatus)
{
  QString errstr = QString::fromUtf8(client.GetErrorStr());
  QString statusMessage;

  if (newStatus == NJClient::NJC_STATUS_OK) {
    clientMutex.lock();
    QString host = QString::fromUtf8(client.GetHostName());
    QString username = QString::fromUtf8(client.GetUserName());
    clientMutex.unlock();

    statusMessage = tr("Connected to %1 as %2").arg(host, username);
  } else if (!errstr.isEmpty()) {
    statusMessage = "Error: " + errstr;
  } else if (newStatus == NJClient::NJC_STATUS_DISCONNECTED) {
    statusMessage = tr("Error: unexpected disconnect");
  } else if (newStatus == NJClient::NJC_STATUS_INVALIDAUTH) {
    statusMessage = tr("Error: authentication failed");
  } else if (newStatus == NJClient::NJC_STATUS_CANTCONNECT) {
    statusMessage = tr("Error: connecting failed");
  }

  chatAddLine(statusMessage, "");

  if (newStatus < 0) {
    Disconnect();
  }
}

void MainWindow::BeatsPerMinuteChanged(int bpm)
{
  if (bpm > 0) {
    bpmLabel->setText(tr("BPM: %1").arg(bpm));
  } else {
    bpmLabel->setText(tr("BPM: N/A"));
  }
}

void MainWindow::BeatsPerIntervalChanged(int bpi)
{
  if (bpi > 0) {
    bpiLabel->setText(tr("BPI: %1").arg(bpi));
  } else {
    bpiLabel->setText(tr("BPI: N/A"));
  }
}

/* Append line with bold formatted prefix to the chat widget */
void MainWindow::chatAddLine(const QString &prefix, const QString &content)
{
  QTextCharFormat oldFormat = chatOutput->currentCharFormat();
  QTextCharFormat boldFormat = oldFormat;
  boldFormat.setFontWeight(QFont::Bold);

  chatOutput->setCurrentCharFormat(boldFormat);
  chatOutput->append(prefix);
  chatOutput->setCurrentCharFormat(oldFormat);
  chatOutput->insertPlainText(content);
}

/* Append a message from a given source to the chat widget */
void MainWindow::chatAddMessage(const QString &src, const QString &msg)
{
  if (src.isEmpty()) {
    chatAddLine("*** ", msg);
  } else if (msg.startsWith("/me ")) {
    chatAddLine(QString("* %1 ").arg(src), msg.mid(4));
  } else {
    chatAddLine(QString("<%1> ").arg(src), msg);
  }
}

void MainWindow::ChatMessageCallback(char **charparms, int nparms)
{
  QString parms[nparms];
  int i;

  for (i = 0; i < nparms; i++) {
    if (charparms[i]) {
      parms[i] = QString::fromUtf8(charparms[i]);
    }
  }

  if (parms[0] == "TOPIC") {
    if (parms[1].isEmpty()) {
      if (parms[2].isEmpty()) {
        chatAddLine("No topic is set.", "");
      } else {
        chatAddLine(QString("Topic is: "), parms[2]);
      }
    } else {
      if (parms[2].isEmpty()) {
        chatAddLine(QString("%1 removes topic.").arg(parms[1]), "");
      } else {
        chatAddLine(QString("%1 sets topic to: ").arg(parms[1]), parms[2]);
      }
    }

    /* TODO set topic */
  } else if (parms[0] == "MSG") {
    chatAddMessage(parms[1], parms[2]);
  } else if (parms[0] == "PRIVMSG") {
    chatAddLine(QString("* %1 * ").arg(parms[1]), parms[2]);
  } else if (parms[0] == "JOIN") {
    chatAddLine(QString("%1 has joined the server").arg(parms[1]), "");
  } else if (parms[0] == "PART") {
    chatAddLine(QString("%1 has left the server").arg(parms[1]), "");
  } else {
    chatOutput->append("Unrecognized command:");
    for (i = 0; i < nparms; i++) {
      chatOutput->append(QString("[%1] %2").arg(i).arg(parms[i]));
    }
  }
}

void MainWindow::ChatInputReturnPressed()
{
  QString line = chatInput->text();
  chatInput->clear();

  QString command, parm, msg;
  if (line.compare("/clear", Qt::CaseInsensitive) == 0) {
    chatOutput->clear();
    return;
  } else if (line.startsWith("/me ", Qt::CaseInsensitive)) {
    command = "MSG";
    parm = line;
  } else if (line.startsWith("/topic ", Qt::CaseInsensitive) ||
             line.startsWith("/kick ", Qt::CaseInsensitive) ||
             line.startsWith("/bpm ", Qt::CaseInsensitive) ||
             line.startsWith("/bpi ", Qt::CaseInsensitive)) {
    command = "ADMIN";
    parm = line.mid(1);
  } else if (line.startsWith("/admin ", Qt::CaseInsensitive)) {
    command = "ADMIN";
    parm = line.section(' ', 1, -1, QString::SectionSkipEmpty);
  } else if (line.startsWith("/msg ", Qt::CaseInsensitive)) {
    command = "PRIVMSG";
    parm = line.section(' ', 1, 1, QString::SectionSkipEmpty);
    msg = line.section(' ', 2, -1, QString::SectionSkipEmpty);
    if (msg.isEmpty()) {
      chatAddLine("error: /msg requires a username and a message.", "");
      return;
    }
    chatAddLine(QString("-> *%1* ").arg(parm), msg);
  } else {
    command = "MSG";
    parm = line;
  }

  clientMutex.lock();
  bool connected = client.GetStatus() == NJClient::NJC_STATUS_OK;
  if (connected) {
    if (command == "PRIVMSG") {
      client.ChatMessage_Send(command.toUtf8().data(), parm.toUtf8().data(), msg.toUtf8().data());
    } else {
      client.ChatMessage_Send(command.toUtf8().data(), parm.toUtf8().data());
    }
  }
  clientMutex.unlock();

  if (!connected) {
    chatAddLine("error: not connected to a server.", "");
  }
}

void MainWindow::MetronomeMuteChanged(bool mute)
{
  clientMutex.lock();
  client.config_metronome_mute = mute;
  clientMutex.unlock();
}

void MainWindow::MetronomeBoostChanged(bool boost)
{
  clientMutex.lock();
  client.config_metronome = boost ? DB2VAL(3) : DB2VAL(0);
  clientMutex.unlock();
}

void MainWindow::LocalChannelMuteChanged(int ch, bool mute)
{
  clientMutex.lock();
  client.SetLocalChannelMonitoring(ch, false, 0, false, 0, true, mute, false, false);
  clientMutex.unlock();
}

void MainWindow::LocalChannelBoostChanged(int ch, bool boost)
{
  clientMutex.lock();
  client.SetLocalChannelMonitoring(ch, true, boost ? DB2VAL(3) : DB2VAL(0),
                                   false, 0, false, false, false, false);
  clientMutex.unlock();
}

void MainWindow::LocalChannelBroadcastChanged(int ch, bool broadcast)
{
  clientMutex.lock();
  client.SetLocalChannelInfo(ch, NULL, false, 0, false, 0, true, broadcast);
  clientMutex.unlock();
}

void MainWindow::RemoteChannelMuteChanged(int useridx, int channelidx, bool mute)
{
  clientMutex.lock();
  client.SetUserChannelState(useridx, channelidx, false, false, false, 0, false, 0, true, mute, false, false);
  clientMutex.unlock();
}
