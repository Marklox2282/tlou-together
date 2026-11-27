#pragma once
// Отправка логов на вебхук Discord - только для разработки: адрес берётся из tlou-together.ini
// (log_webhook=...), в моде и в репозитории его нет. Без адреса ничего не отправляется.
#include <string>

namespace logsend {

// Адрес вебхука из ini (пусто - выключено). Принимаются только https://discord.com/api/webhooks/...
void SetWebhook(const std::string& url);
bool Enabled();
std::string Webhook();  // адрес (пусто - выключено)
// При запуске игры: лог прошлого запуска (tlou-together.prev.log) и файл падений, если он новый.
void SendPreviousSession(unsigned modVersion);
// По F11: текущий лог и файл падений прямо сейчас.
void SendNow(unsigned modVersion, const std::string& note);

}  // namespace logsend
