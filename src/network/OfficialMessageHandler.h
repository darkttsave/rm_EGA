// SPDX-License-Identifier: MIT
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <functional>

class GameData;

namespace RM {

class OfficialMessageHandler {
public:
  enum class Result { Unsupported, Parsed, ParseFailed };
  using GameStatusCacheUpdater = std::function<void(quint32, quint16, bool)>;
  using ParsedLogger = std::function<void(const QString &, const QJsonObject &)>;

  // The optional logger runs before GameData is updated, as in NetworkManager.
  static Result handle(const QString &topic, const QByteArray &payload,
                       GameData &gameData, const ParsedLogger &logger = {},
                       const GameStatusCacheUpdater &cacheUpdater = {});
};
} // namespace RM
