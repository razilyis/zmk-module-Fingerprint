# TouchPass ZMK Module

> [!NOTE]
> 本プロジェクトは、[tobiasweissert/TouchPass](https://github.com/tobiasweissert/TouchPass) をベースに ZMK モジュールとして再構成したものです。
> オリジナルの素晴らしいアイデアと実装に敬意を表します。

> [!WARNING]
> **免責事項**: 本プログラムの使用は自己責任でお願いします。本プログラムを使用したことによる不利益や損害について、作者は一切責任を負いません。

TouchPass は、指紋センサー（R502-A 等）を使用してパスワードを管理・入力するための ZMK モジュールです。
指紋認証に成功すると、事前に登録されたパスワードをキーストロークとして自動送信します。

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
    - name: zmk-module-touchpass
      remote: TouchPass # 指紋センサーモジュールを指定
      revision: main
```

> [!IMPORTANT]
> GitHub Actions でビルドする場合、`razilyis` リポジトリに本モジュールがプッシュされている必要があります。また、上記のように `remotes` セクションでの定義も必須です。

### 2. Kconfig 設定の有効化

`config/board.conf`（または `shield.conf`）に以下の設定を追加します。

```kconfig
# TouchPass を有効化
CONFIG_ZMK_TOUCHPASS=y
# 設定ツール (config.html) 用のシリアル通信を有効化
CONFIG_ZMK_TOUCHPASS_SERIAL_RPC=y
# 常時待機モード（指を置くだけで入力）を有効化する場合（デフォルト: n）
# CONFIG_ZMK_TOUCHPASS_ALWAYS_ON=y

# 必要なサブシステムの有効化
CONFIG_USB_DEVICE_STACK=y
CONFIG_UART_LINE_CTRL=y
CONFIG_USB_CDC_ACM=y
CONFIG_UART_INTERRUPT_DRIVEN=y
CONFIG_FILE_SYSTEM=y
```

### 3. デバイスツリー (.overlay) の設定

指紋センサーを接続する UART ピンを定義し、Behavior をインスタンス化します。
以下は XIAO nRF52840 (D6/TX, D7/RX) の例です。

```dts
// 共通定義の読み込み
/ {
    behaviors {
        tp: tp {
            compatible = "zmk,behavior-touchpass";
            #binding-cells = <0>;
        };
    };
};

// ピンアサインの設定 (ボードに合わせて変更してください)
&pinctrl {
    uart0_default: uart0_default {
        group1 {
            psels = <NRF_PSEL(UART_TX, 1, 11)>, // TX Pin
                    <NRF_PSEL(UART_RX, 1, 12)>; // RX Pin
        };
    };
};

&uart0 {
    status = "okay";
    current-speed = <57600>;
    pinctrl-0 = <&uart0_default>;
    pinctrl-names = "default";
};
```

### 4. 配線 (Wiring)

R502-A センサーを以下のように接続します。**電圧は 3.3V** を使用してください。

| センサー端子 | 接続先 (例: XIAO nRF52840) | 役割 |
| :--- | :--- | :--- |
| **VCC (Red)** | **3.3V** | 電源 (3.3V 専用) |
| **GND (Black)** | **GND** | グラウンド |
| **TX (Yellow)** | **D7 (P1.12 / RX)** | データ送信 -> MCUのRXへ |
| **RX (White)** | **D6 (P1.11 / TX)** | データ受信 <- MCUのTXへ |

> [!CAUTION]
> センサーの TX を MCU の TX に繋がないよう注意してください（クロス接続が必要です）。

### 5. キーマップでの使用

`config/your_keyboard.keymap` で、定義した `&tp` をキーに割り当てます。

```dts
/ {
    keymap {
        default_layer {
            bindings = <
                &kp A &kp B &tp  // &tp を押すと指紋認証待ちになります
            >;
        };
    };
};
```

## 設定ツール
ビルドしたファームウェアを書き込んだ後、USB で PC に接続し、[config.html](./config.html) をブラウザで開くことで、指紋の登録やパスワードの設定が可能です。

