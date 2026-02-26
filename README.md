# TouchPass for ZMK Module

> [!NOTE]
> 本プロジェクトは、[tobiasweissert/TouchPass](https://github.com/tobiasweissert/TouchPass) をベースに ZMK モジュールとして再構成したものです。
> オリジナルの素晴らしいアイデアと実装に敬意を表します。

> [!WARNING]
> **免責事項**: 本プログラムの使用は自己責任でお願いします。本プログラムを使用したことによる不利益や損害について、作者は一切責任を負いません。

> [!WARNING]
> **セキュリティに関する注意**: 登録したパスワードはフラッシュメモリに**暗号化なしで平文保存**されます。
> また、設定ツール（Web Serial）との通信やファームウェアの読み出しによって、パスワードが第三者に漏洩する可能性があります。
> 本モジュールの使用は、物理的なセキュリティが確保された環境を前提としています。

TouchPass は、指紋センサー（R502-A 等）を使用してパスワードを管理・入力するための ZMK モジュールです。
本モジュールは **ZMK v0.3 (Zephyr 3.5)** をベースに構築・検証されています。
指紋認証に成功すると、事前に登録されたパスワードをキーストロークとして自動送信します。

### 主な特徴
- **非同期・ノンブロッキング動作**: センサーの初期化や指紋認証処理は専用の独立したスレッドで行われます。そのため、認証待ち中や長いパスワードを送信している最中でも、通常のタイピング（他のキー入力）やレイヤー切り替えが阻害されることはありません。
- **自動リカバリ (起動待機)**: キーボード起動直後に指紋センサーの応答がない場合でも諦めず、バックグラウンド接続のリトライを維持します。後から線が繋がった場合でも、自動的に復帰して利用可能になります。
- **Web対応設定ツール**: Web Serial API対応ブラウザ（Chrome/Edge等）から、GitHub Pages で公開されている設定ツール、またはリポジトリの [index.html](./index.html) を開き、パスワードや指紋の管理が可能です。（登録名は最大31文字、パスワードは最大63文字まで対応）

## 設定ツール (index.html)
ビルドしたファームウェアを書き込んだ後、USB で PC に接続し、以下のいずれかの方法で設定ツールを開きます。

- **GitHub Pages (推奨)**: [https://razilyis.github.io/zmk-module-Fingerprint/](https://razilyis.github.io/zmk-module-Fingerprint/)
- **ローカルファイル**: リポジトリ内の [index.html](./index.html) をブラウザで開く

## 他のキーボードへの導入方法

既存の ZMK 構成（zmk-config）に TouchPass を追加するには、以下の手順に従ってください。

### 1. west.yml への追加

`config/west.yml` に `remotes` と `projects` を追加します。

```yaml
manifest:
  remotes:
    - name: TouchPass
      url-base: https://github.com/razilyis

  projects:
    - name: zmk-module-Fingerprint
      remote: TouchPass # 指紋センサーモジュールを指定
      revision: main
```

> [!IMPORTANT]
> GitHub Actions でビルドする場合、`razilyis` リポジトリに本モジュールがプッシュされている必要があります。また、上記のように `remotes` セクションでの定義も必須です。

### 2. Kconfig 設定の有効化

`config/board.conf`（または `shield.conf`）に以下の設定を追加します。

```kconfig
# TouchPass を有効化（SERIAL サブシステムが必要）
CONFIG_ZMK_TOUCHPASS=y
CONFIG_SERIAL=y

# 設定ツール用のシリアル通信を有効化
CONFIG_ZMK_TOUCHPASS_SERIAL_RPC=y

# 常時待機モード（指を置くだけで入力）を有効化する場合（デフォルト: n）
# CONFIG_ZMK_TOUCHPASS_ALWAYS_ON=y

# 必要なサブシステムの有効化
CONFIG_USB_DEVICE_STACK=y
CONFIG_UART_LINE_CTRL=y
CONFIG_USB_CDC_ACM=y
CONFIG_UART_INTERRUPT_DRIVEN=y
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_FLASH_PAGE_LAYOUT=y
CONFIG_SETTINGS=y
CONFIG_NVS=y
CONFIG_SETTINGS_NVS=y
CONFIG_REBOOT=y

# Serial RPC を使う場合の競合回避（JSON破損防止）
CONFIG_ZMK_USB_LOGGING=n
CONFIG_LOG_BACKEND_UART=n
CONFIG_UART_CONSOLE=n
```

#### オプション設定

| Kconfig | デフォルト | 説明 |
|---|---|---|
| `CONFIG_ZMK_TOUCHPASS_ENROLL_TIMEOUT_S` | `60` | 登録タイムアウト（秒）。範囲: 10〜300 |
| `CONFIG_ZMK_TOUCHPASS_POLL_INTERVAL_MS` | `80` | 常時待機モードのポーリング間隔（ms）。範囲: 50〜500。`ALWAYS_ON` 有効時のみ有効 |

### 3. デバイスツリー (.overlay) と キーマップ (.keymap) の設定

#### 3-1. キーマップ (.keymap) の設定

通常は、利用するキーマップファイルの先頭に指紋センサー用の Behavior 定義 (`touchpass.dtsi`) をインクルードします。

```dts
#include <behaviors/touchpass.dtsi>
```

これにより、キーマップ内で `&touchpass` が利用可能になります。

> [!TIP]
> **Keymap Editor (GUI) への対応**  
> GUI ベースの Keymap Editor を使用する場合、外部モジュールのインクルードファイル内にある Behavior は認識されない（候補に表示されない）ことがあります。その場合は、`#include` を使わずに、以下の定義を直接 `.keymap` ファイルの `behaviors` ブロック内に記述してください。
>
> ```dts
> / {
>     behaviors {
>         touchpass: touchpass {
>             compatible = "zmk,behavior-touchpass";
>             #binding-cells = <0>;
>         };
>     };
> };
> ```

#### 3-2. デバイスツリー (.overlay) の設定

続いてボード用のオーバーレイファイルで、以下を定義します。
- 指紋センサーを接続する UART ピン
- (オプション) 設定ツール用の CDC ACM ノード
- NVS (Flash) 領域の調整

以下は XIAO nRF52840 (D6/TX, D7/RX) の例です。

```dts
// ピンアサインの設定 (ボードに合わせて変更してください)
&pinctrl {
    uart0_default: uart0_default {
        group1 {
            psels = <NRF_PSEL(UART_TX, 1, 11)>, // TX Pin (D6)
                    <NRF_PSEL(UART_RX, 1, 12)>; // RX Pin (D7)
        };
    };
    uart0_sleep: uart0_sleep {
        group1 {
            psels = <NRF_PSEL(UART_TX, 1, 11)>,
                    <NRF_PSEL(UART_RX, 1, 12)>;
            low-power-enable;
        };
    };
};

&uart0 {
    status = "okay";
    current-speed = <57600>;
    pinctrl-0 = <&uart0_default>;
    pinctrl-1 = <&uart0_sleep>;
    pinctrl-names = "default", "sleep";
};

// Serial RPC を使用する場合は以下も追加
&zephyr_udc0 {
    status = "okay";
    usb_cdc_acm_uart: cdc-acm-uart {
        compatible = "zephyr,cdc-acm-uart";
    };
};

/ {
    chosen {
        /delete-property/ zephyr,console;
        /delete-property/ zephyr,shell-uart;
        zephyr,console = &usb_cdc_acm_uart;
        zephyr,shell-uart = &usb_cdc_acm_uart;
    };
};

/* NVS partitions
 * - touchpass_partition: 指紋データ保存用
 * - storage_partition:   ZMK settings/BLE bonding 用
 */
&flash0 {
    partitions {
        compatible = "fixed-partitions";
        #address-cells = <1>;
        #size-cells = <1>;

        touchpass_partition: partition@e4000 {
            label = "touchpass";
            reg = <0x000e4000 0x00008000>;
        };

        storage_partition: partition@ec000 {
            label = "storage";
            reg = <0x000ec000 0x00008000>;
        };
    };
};
```

### 4. 配線 (Wiring)

R502-A センサーを以下のように接続します。**電圧は 3.3V** を使用してください。

| センサー端子 | 接続先 (例: XIAO nRF52840) | 役割 |
| :--- | :--- | :--- |
| **VCC (Red)** | **3.3V** | 電源 (3.3V 専用) |
| **GND (Black)** | **GND** | グラウンド |
| **TX (Yellow)** | **D7 (P1.12 / RX)** | データ送信 → MCU の RX へ |
| **RX (Green)** | **D6 (P1.11 / TX)** | データ受信 ← MCU の TX へ |

> [!CAUTION]
> センサーの TX を MCU の TX に繋がないよう注意してください（クロス接続が必要です）。

### 5. キーマップでの使用

ご自身の `config/your_keyboard.keymap` の任意のキー位置に `&touchpass` を割り当ててください。

**設定例：**
```dts
#include <behaviors/touchpass.dtsi> // ファイルの先頭に追記

// ... 他の設定 ...

            bindings = <
                // ... 他のキー群 ...
                &kp A  &kp B  &touchpass  // 指紋認証センサーを配置したい位置に &touchpass を記述
                // ... 他のキー群 ...
            >;
```

#### `&touchpass` の動作仕様
キーボード上で `&touchpass` を割り当てたキーを押下すると、以下の処理が非同期で実行されます：
1. **待機開始**: 指紋センサーが青色等に点灯（または点滅）し、指紋の読み取り待機状態になります。
2. **パスワード自動入力**: 登録済みの指を乗せて認証に成功した場合、センサー内部に保存された対応するパスワードが USB 経由でキーストロークとして即座に自動送信されます（末尾に Enter が付きます）。
3. **ノンブロッキング処理**: センサーが指紋を待機している間でも、キーボードの他のキー入力やレイヤー切り替えは一切阻害されません。通常通りタイピングを継続できます。
4. **自動タイムアウト**: 一定時間（おおむね数秒以内）指が置かれなかった場合や、認証に失敗した場合は、待機状態が自動でキャンセルされます（特別なキャンセル操作は不要です）。

## 設定ツールの使用方法

1. [GitHub Pages](https://razilyis.github.io/zmk-module-Fingerprint/) を開くか、ローカルの `index.html` をブラウザで開きます。
2. 指紋の登録（6ステップキャプチャ）
3. 登録済み指紋へのパスワード設定・変更・削除
4. BLE ボンド削除・再起動
5. センサー・接続状態の確認

> [!IMPORTANT]
> **ブラウザ要件**: Web Serial API を使用するため、**Google Chrome または Microsoft Edge 89 以降**が必要です。Firefox・Safari では動作しません。
> ページを開いた後、「Connect」ボタンをクリックしてキーボードのシリアルポートを選択してください。

## トラブルシューティング

| 症状 | 原因候補 | 対処 |
|---|---|---|
| 接続できない | `CONFIG_ZMK_TOUCHPASS_SERIAL_RPC=n` | `CONFIG_ZMK_TOUCHPASS_SERIAL_RPC=y` を設定してリビルド |
| 接続できない | `CONFIG_ZMK_USB_LOGGING=y` と競合 | `CONFIG_ZMK_USB_LOGGING=n` に設定 |
| センサーが認識されない | UART ピン設定ミス | `.overlay` のピン番号と実際の配線を確認 |
| センサーが認識されない | 電源電圧 | R502-A は **3.3V 専用**。5V 接続は破損の原因 |
| 登録がタイムアウトする | デフォルトの60秒では短い | `CONFIG_ZMK_TOUCHPASS_ENROLL_TIMEOUT_S=120` 等に延長 |
| BLE 接続が不安定 | BLE bonding 情報の不整合 | `settings_reset` ファームウェアで Flash 初期化後、再ペアリング |
| パスワードが正しく入力されない | パスワードに未対応の記号が含まれる | 現在 US キーボードレイアウトのみ対応。日本語IME 入力は不可 |

## ライセンス (License)

本プロジェクトは **MIT License** の下で公開されています。

- 原作者: [Tobias Weissert](https://github.com/tobiasweissert)
- 改変・ZMKモジュール化: [RaZiLy](https://github.com/razilyis)

詳細は [LICENSE](./LICENSE) ファイルを参照してください。
