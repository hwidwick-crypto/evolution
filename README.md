# Evolution loader

Консольный лоудер, который маппит кернел-драйвер **Evolution** в ядро Windows с помощью
[kdmapper](kdmapper-master) (украдённый iqvw64e / Intel driver exploit).

```
##### #   #  ###  #     #   # ##### #####  ###  #   #
#     #   # #   # #     #   #   #     #   #   # ##  #
####  #   # #   # #     #   #   #     #   #   # # # #
#      # #  #   # #     #   #   #     #   #   # #  ##
#####   #    ###  #####  ###    #   #####  ###  #   #

        evolution | kernel loader

1 - Start
2 - Exit

>
```

## Сборка

* `build.cmd` — просто запустить, либо
* открыть `Evolution.sln` в Visual Studio 2022 и `Build → Build Solution` (конфигурация `Release | x64`).

Собираются два проекта:

| Проект | Результат | Что это |
|---|---|---|
| `loader` | `output\x64\Release\Evolution.exe` | консольный лоудер (манифест `requireAdministrator`, UAC спросит сам) |
| `driver` | `output\x64\Release\evolution.sys` | собственный кернел-драйвер `Evolution` (WDM) |

Для драйвера **не нужен установленный WDK и права администратора**: `build.cmd` один раз
скачивает официальный пакет `Microsoft.Windows.WDK.x64` (с nuget.org, ~106 МБ) и распаковывает
его в `wdk\nuget` (это делает `setup-wdk.ps1`), после чего проект просто берёт оттуда
`wdm.h` / `ntoskrnl.lib`. kdmapper компилируется прямо внутрь лоудера (`KDLIBMODE` +
`DISABLE_OUTPUT`), поэтому весь вывод в консоль — лоудерный, с цветами.

### Драйвер (`driver/`)

* обычный WDM-драйвер: `DriverEntry` создаёт `\Device\Evolution` + `\DosDevices\Evolution`,
  то есть пользовательское устройство **`\\.\Evolution`** (его же лоудер и проверяет);
* IOCTL `EVOLUTION_IOCTL_PING` (buffered) отвечает магией `0xE70110` — проверка «драйвер жив»;
* работает **и как обычный сервис-драйвер** (DriverObject от I/O менеджера),
  **и как ручной маппинг**: kdmapper зовёт `DriverEntry(NULL, NULL)`, поэтому в этом случае
  драйвер сам просит ядро создать объект через `IoCreateDriver(L"\\Driver\\Evolution", ...)`
  — без этого `IoCreateDevice`/открытие `\\.\Evolution` невозможны;
* импорты только из `ntoskrnl.exe` (ровно то, что kdmapper умеет резолвить):
  `IoCreateDriver`, `IoCreateDevice`, `IoCreateSymbolicLink`, `IoDeleteDevice`,
  `IoDeleteSymbolicLink`, `IofCompleteRequest`, `RtlInitUnicodeString`, `DbgPrintEx`;
* `/GS-` (как в примерах WDK) и без CRT — карта драйвера минимальна.

## Использование

1. Запустить `Evolution.exe` → нажать `1` (Start). `evolution.sys` собирается автоматически
   рядом с лоудером, класть его руками не нужно.
2. Логи:

```
[~] checking driver...                       (серый)   — проверка, загружен ли драйвер
[+] driver ready!                            (зелёный) — уже загружен, маппить не нужно
[~] driver not loaded, mapping...            (серый)   — драйвера нет, идём маппить
[~] loading vulnerable intel driver...       (серый)
[~] mapping evolution.sys...                 (серый)
[+] driver mapped successfully               (зелёный)
[+] driver ready!                            (зелёный)

[!] driver mapping failed                    (красный) — маппинг не удался
<причина>                                    (жёлтый)
make sure core isolation is disabled & game is closed   (жёлтый)
```

После любого результата (успех или ошибка) консоль остаётся открытой и возвращается в меню.
Клавиша `2` — выход.

## Как определяется, загружен ли драйвер

`loader/main.cpp`, константы вверху файла:

```cpp
kDriverFileName = L"evolution.sys";  // файл драйвера рядом с лоудером
kDevicePath     = L"\\\\.\\Evolution"; // устройство, которое драйвер создаёт в DriverEntry
kServiceName    = L"Evolution";        // имя сервиса (если драйвер ставится как сервис)
```

Проверка: `CreateFile(\\.\Evolution)` — если открылось (или ошибка не `ERROR_FILE_NOT_FOUND`),
драйвер уже в системе; иначе fallback — состояние сервиса `Evolution` в SCM.
Если твой драйвер называет устройство иначе — поменяй `kDevicePath`.

## Про сборку на этой машине

В `Directory.Build.props` лежит фикс для этой конкретной установки VS: дефолтный v143
toolset (`14.39.33519`) остался без x64-библиотек и любая линковка падала с
`LNK1104: msvcprt.lib`. Файл сам определяет неполный toolset и переключает `VCToolsVersion`
на тот, что Visual Studio указывает как дефолтный (в healthy-установках ничего не меняет).

## Важно

* нужен запуск **от администратора**;
* **Core isolation / Memory integrity** должен быть выключен (иначе блоклист ловит iqvw64e);
* игра/античит должна быть закрыта;
* антивирус может ругаться на kdmapper — это ожидаемо.
