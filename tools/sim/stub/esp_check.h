#pragma once
#define ESP_RETURN_ON_FALSE(x, err, tag, ...) do { if (!(x)) return (err); } while (0)
#define ESP_GOTO_ON_FALSE(x, err, label, tag, ...) do { if (!(x)) { ret = (err); goto label; } } while (0)
