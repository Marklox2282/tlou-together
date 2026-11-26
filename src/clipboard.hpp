#pragma once
// Буфер обмена Windows: код лобби хозяин отдаёт туда, гость берёт оттуда (в меню игры нет поля ввода).

#include <string>

namespace clipboard {

std::string Text();                    // текст (только ASCII, до 64 символов); пусто - нет текста
void Set(const std::string& text);

}  // namespace clipboard
