#pragma once

#include <map>
#include <QObject>

#include "tools/cabana/dbc/dbc.h"

const QString AUTO_SAVE_EXTENSION = ".tmp";

class DBCFile : public QObject {
  Q_OBJECT

public:
  DBCFile(const QString &dbc_file_name, QObject *parent=nullptr);
  DBCFile(const QString &name, const QString &content, QObject *parent=nullptr);
  ~DBCFile() {}

  void open(const QString &content);

  bool save();
  bool saveAs(const QString &new_filename);
  bool autoSave();
  bool writeContents(const QString &fn);
  void cleanupAutoSaveFile();
  QString generateDBC();

  void updateMsg(const MessageId &id, const QString &name, uint32_t size, const QString &comment);
  inline void removeMsg(const MessageId &id) { msgs.erase(id.address); }

  inline const std::map<uint32_t, cabana::Msg> &getMessages() const { return msgs; }
  cabana::Msg *msg(uint32_t address);
  cabana::Msg *msg(const QString &name);
  inline cabana::Msg *msg(const MessageId &id) { return msg(id.address); }

  int signalCount();
  inline int msgCount() { return msgs.size(); }
  inline QString name() { return name_.isEmpty() ? "untitled" : name_; }
  inline bool isEmpty() { return (signalCount() == 0) && name_.isEmpty(); }

  QString filename;

private:
  void parseExtraInfo(const QString &content);
  std::map<uint32_t, cabana::Msg> msgs;
  QString name_;
};
