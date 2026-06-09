/*
 Copyright (C) 2025 Kristian Duske

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <QObject>

class QHttpServer;
class QTcpServer;

namespace tb::ui
{
class AppController;

/**
 * Loopback HTTP control server for external tools (see HTTP_API_RESEARCH.md).
 *
 * Runs Qt's QHttpServer on the GUI thread and binds to 127.0.0.1 only, so route
 * handlers reach the live model directly with no marshaling and each request is
 * atomic with respect to UI input. This is the v1 scaffold; it serves a single
 * read-only route, GET /documents.
 */
class ApiServer : public QObject
{
  Q_OBJECT
public:
  static constexpr quint16 defaultPort = 28196;

  explicit ApiServer(AppController& appController, QObject* parent = nullptr);
  ~ApiServer() override;

  /**
   * Binds to 127.0.0.1 on the given port (0 lets the OS choose a free one) and
   * starts serving. Returns the bound port, or 0 if binding failed. Logs the
   * outcome either way.
   */
  quint16 start(quint16 port = defaultPort);

  quint16 port() const;

private:
  void registerRoutes();

  AppController& m_appController;
  QHttpServer* m_httpServer = nullptr;
  QTcpServer* m_tcpServer = nullptr;
  quint16 m_port = 0;
};

} // namespace tb::ui
