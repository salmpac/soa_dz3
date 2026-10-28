#pragma once

#include <library/cpp/yt/error/error.h>

#include <functional>

namespace NSoa {

////////////////////////////////////////////////////////////////////////////////

//! Общая инициализация процесса сервиса:
//!  - логирование в stderr (уровень из LOG_LEVEL, по умолчанию info);
//!  - резолвер адресов с IPv4 (по умолчанию YT резолвит только IPv6).
NYT::TError ConfigureProcess();

//! Запускает #main в fiber'е на отдельной очереди и блокирует поток до её завершения.
//! Ошибку логирует; возвращает код выхода процесса.
int RunInFiber(std::function<NYT::TError()> main);

//! ConfigureProcess + RunInFiber: точка входа утилиты, которая отрабатывает и завершается.
int RunProgram(std::function<NYT::TError()> main);

//! Точка входа долгоживущего сервиса.
/*!
 *  1. Блокирует SIGTERM/SIGINT до создания каких-либо потоков: маску наследуют
 *     все потоки YT, поэтому сигнал достаётся только главному потоку через sigwait.
 *     (В контейнере сервис - PID 1, и без этого SIGTERM просто игнорировался бы.)
 *  2. ConfigureProcess.
 *  3. #start в fiber'е: поднимает сервер и возвращается.
 *  4. Главный поток спит в sigwait до SIGTERM/SIGINT.
 *  5. #stop в fiber'е (graceful остановка сервера), затем NYT::Shutdown().
 *
 *  Возвращает код выхода процесса.
 */
int RunService(std::function<NYT::TError()> start, std::function<NYT::TError()> stop);

////////////////////////////////////////////////////////////////////////////////

} // namespace NSoa
