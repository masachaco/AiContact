# ACI — AI Contact Instrument Format v1

ACI (AI Contact Instrument) は、**単一ファイルで配布できる、ソフトウェア音源のための標準フォーマット**です。
VST/CLAP がネイティブコードの ABI を定めているのと同じように、ACI は **WebAssembly モジュールの ABI** を定めます。

- **1 音源 = 1 ファイル** (`*.wasm`、推奨拡張子 `*.aci.wasm`)
- コンテナは W3C 標準の **WebAssembly バイナリ**。C / C++ / Rust / Zig / AssemblyScript など、wasm を出力できる言語ならどれでも書ける
- **サンドボックス化**されており、音源がホスト（DAW）をクラッシュさせたりファイルを読み書きしたりできない
- **関数 6 個 + メタデータ JSON** だけで成立する小さな ABI。AI が 1 ファイルで書き切れる規模を想定
- 画像や HTML を使った**独自の GUI** も同じファイルに同梱できる（§10、任意）
- ホストはファイルを差し替えるだけで**ホットリロード**できる（ネイティブの DLL と違い、アンロード時の問題がない）

本文書のキーワード「MUST / SHOULD / MAY」は RFC 2119 の意味で用います。

> **開発中の仕様です。** 正式リリースまでは互換性を保たない変更を行うことがあります（`aci` の値は当面 `1` のまま）。

---

## 1. ファイル

| 項目 | 規定 |
|---|---|
| 形式 | WebAssembly バイナリ (Core spec 2.0)。SIMD・threads・GC・例外は使用不可 (MUST NOT) |
| 拡張子 | `.wasm`（`<name>.aci.wasm` を推奨） |
| サイズ | 16 MiB 以下 (SHOULD)。サンプル等はデータセグメントとして埋め込む |
| メモリ | `memory` という名前で線形メモリを 1 つエクスポートする (MUST)。最大 256 MiB |
| インポート | `env.aci_log` **のみ**許可 (MAY)。それ以外のインポートを持つモジュールはホストが拒否する (MUST) |

インポートを 1 つに制限しているのは、**どの ACI ホストでも必ず同じ音が鳴る**ことを保証するためです。
数学関数（`sinf`、`powf`…）やメモリ関数（`memcpy`…）はモジュール内に含めます。C の場合は
`sdk/c/aci.h` をインクルードするだけで自動的に含まれます。Rust / Zig / AssemblyScript は標準で自己完結した wasm を出力します。

## 2. エクスポート関数

wasm の型表記: `i32` = 32bit 整数（ポインタを含む）、`f32` = 単精度浮動小数点。

### 必須 (MUST)

| 名前 | シグネチャ | 説明 |
|---|---|---|
| `aci_abi_version` | `() -> i32` | 常に `1` を返す |
| `aci_meta` | `() -> i32` | メタデータ JSON（UTF-8、NUL 終端）へのポインタ。64 KiB 以下 |
| `aci_init` | `(sample_rate: f32, max_frames: i32) -> i32` | 初期化。成功時 `0`、失敗時は非 0 |
| `aci_output` | `(channel: i32) -> i32` | 出力バッファ (`f32[max_frames]`) へのポインタ。`0` = L、`1` = R |
| `aci_process` | `(frames: i32) -> ()` | `frames` サンプル分 (`1 ≤ frames ≤ max_frames`) を出力バッファに書く |
| `aci_note_on` | `(key: i32, velocity: f32) -> ()` | key は MIDI ノート番号 0–127、velocity は 0–1（0 より大） |
| `aci_note_off` | `(key: i32, velocity: f32) -> ()` | リリース。velocity はリリースベロシティ 0–1 |

### 任意 (MAY)

| 名前 | シグネチャ | 説明 |
|---|---|---|
| `aci_set_param` | `(id: i32, value: f32) -> ()` | パラメータ設定。value は `min..max` の**実値**（正規化値ではない） |
| `aci_pitch_bend` | `(value: f32) -> ()` | -1 … +1（中央 0）。ベンド幅はモジュールが決める |
| `aci_control_change` | `(cc: i32, value: f32) -> ()` | MIDI CC。value は 0–1 (`raw / 127`) |
| `aci_channel_pressure` | `(value: f32) -> ()` | チャンネルアフタータッチ 0–1 |
| `aci_all_notes_off` | `() -> ()` | 全ボイスをリリース（またはミュート） |
| `aci_reset` | `() -> ()` | 内部状態（ボイス、ディレイ、フィルタ状態）を初期化。パラメータ値は保持 |

エクスポートされていない任意関数に相当するイベントは、ホストが単に送りません。

## 3. ホストインポート

| モジュール.名前 | シグネチャ | 説明 |
|---|---|---|
| `env.aci_log` | `(ptr: i32, len: i32) -> ()` | デバッグログ（UTF-8）。ホストはレート制限してよい |

## 4. メタデータ JSON

`aci_meta()` が返す JSON オブジェクト。

```json
{
  "aci": 1,
  "id": "com.example.supersaw",
  "name": "SuperSaw",
  "version": "1.2.0",
  "author": "you",
  "description": "7-voice detuned saw pad",
  "tags": ["pad", "synth"],
  "outputs": 2,
  "polyphony": 16,
  "host_sustain": true,
  "params": [
    { "id": 0, "name": "Cutoff",  "min": 20, "max": 20000, "default": 2000, "unit": "Hz", "scale": "log" },
    { "id": 1, "name": "Detune",  "min": 0,  "max": 1,     "default": 0.3 },
    { "id": 2, "name": "Wave",    "min": 0,  "max": 2,     "default": 0,    "steps": 3, "labels": ["Saw", "Square", "Tri"] }
  ]
}
```

| キー | 型 | 必須 | 説明 |
|---|---|---|---|
| `aci` | int | ✔ | ABI バージョン。`1` |
| `id` | string | ✔ | 逆ドメイン形式の一意な ID。ホットリロード時のパラメータ引き継ぎに使う |
| `name` | string | ✔ | 表示名 |
| `version` | string | | semver 推奨 |
| `author` / `description` | string | | |
| `tags` | string[] | | `"bass"`, `"pad"`, `"drums"`, `"keys"`, `"fx"` など |
| `outputs` | int | | `1`（モノラル）または `2`（ステレオ、既定）。`1` の場合ホストは `aci_output(0)` を両チャンネルに使う |
| `polyphony` | int | | 情報のみ |
| `host_sustain` | bool | | 既定 `true`。ホストがサステインペダル (CC64) を処理し、ペダル中の note off を遅延させる。`false` の場合 CC64 は `aci_control_change` でモジュールに渡される |
| `params` | object[] | | パラメータ定義（下表） |

パラメータ定義:

| キー | 型 | 必須 | 説明 |
|---|---|---|---|
| `id` | int | ✔ | 0 以上の一意な整数。**バージョンを跨いで同じ意味を保つ** (SHOULD) |
| `name` | string | ✔ | 表示名 |
| `min` / `max` | number | ✔ | 実値の範囲 |
| `default` | number | ✔ | 既定値 |
| `unit` | string | | `"Hz"`, `"s"`, `"dB"`, `"%"` など |
| `scale` | string | | `"linear"`（既定）/ `"log"`。ホスト UI・オートメーションでのカーブ |
| `steps` | int | | 離散値の個数（例: 3 → 0,1,2） |
| `labels` | string[] | | `steps` 個の表示名 |

## 5. ライフサイクル

```
instantiate (start 関数があれば実行)
  → aci_abi_version() == 1 を確認
  → aci_meta() を読む
  → aci_init(sample_rate, max_frames)            … 1 インスタンスにつき 1 回だけ
  → aci_output(0) / aci_output(1) を取得
  → 全パラメータについて aci_set_param(id, value)  … 既定値または保存値
  → ループ { イベント関数… ; aci_process(n) }
  → 破棄
```

- サンプルレートが変わる場合、ホストは**新しいインスタンスを作る** (MUST)。`aci_init` が 2 回呼ばれることはない
- `max_frames` は **4096 以下** (MUST)。モジュールは `float out[4096]` を静的確保すれば十分
- `aci_output` が返すポインタは、インスタンスの寿命の間ずっと有効でなければならない (MUST)
- すべての呼び出しは**単一スレッドで直列**に行われる。モジュールはスレッド安全性を考慮しなくてよい

## 6. タイミングとリアルタイム性

- ホストはオーディオブロックをイベント位置で分割する。イベント関数は、直後の `aci_process` の**先頭サンプル**から効果を持つ（= サンプル精度）
- `aci_process` は出力バッファの `[0, frames)` を**上書き**する (MUST)（加算ではない）
- `aci_process` と各イベント関数は有界時間で終わらなければならない (MUST)。ホストは実行量の上限（ガス）を設け、超えたインスタンスを停止してよい
- `aci_process` 中に `memory.grow` すべきでない (SHOULD NOT)。必要なメモリは `aci_init` までに確保する
- 出力は名目上 -1 … +1。NaN / Inf を出力したモジュールはホストによりミュートされうる

## 7. MIDI からの変換（ホストの責務）

| MIDI | ACI 呼び出し |
|---|---|
| Note On (vel > 0) | `aci_note_on(key, vel / 127)` |
| Note Off / Note On (vel = 0) | `aci_note_off(key, vel / 127)`（サステイン中は遅延） |
| Pitch Bend | `aci_pitch_bend((raw - 8192) / 8192)` |
| CC 64 | `host_sustain` が true ならホスト処理、false なら `aci_control_change` |
| CC 120 / 123 | `aci_all_notes_off()` |
| その他の CC | `aci_control_change(cc, raw / 127)` |
| Channel Pressure | `aci_channel_pressure(raw / 127)` |

MIDI チャンネルによるルーティング・キーレンジ・トランスポーズはホスト側の機能であり、モジュールは関知しない。

## 8. 適合性チェック

ホストはロード時に次を検証し、満たさないモジュールを拒否する (MUST):

1. wasm として妥当で、インポートが `env.aci_log (i32, i32) -> ()` 以外に無い
2. `memory` と必須関数 7 個が、規定のシグネチャでエクスポートされている
3. `aci_abi_version() == 1`、`aci_meta()` が妥当な JSON で必須キーを持つ
4. `aci_init()` が 0 を返し、`aci_output()` の領域が線形メモリ内に収まる

`aicontact-host validate` と API の `instrument.validate` は、加えて試奏レンダリングを行い
NaN・無音・クリップ・DC オフセット・CPU 負荷を報告します。

## 9. バージョニング

- 正式リリースまでは開発中の仕様として、非互換な変更も `aci` の値を上げずに行う
- 正式リリース後は、ABI の非互換な変更で `aci` の値を上げる。ホストは対応していないバージョンを拒否する
- ホストは未知のメタデータキーと未知のカスタムセクションを無視する (MUST)

## 10. カスタム UI（任意）

音源は自前の GUI を同梱できます (MAY)。UI のファイルは wasm の**カスタムセクション**（セクション ID 0）として
同じファイルに埋め込むため、**1 音源 = 1 ファイル**のまま配布できます。カスタムセクションは wasm ランタイムに
無視されるので、UI 非対応のホストでも同じファイルがそのまま動きます（既定の UI が表示される）。

| 項目 | 規定 |
|---|---|
| セクション名 | `aci.ui/<パス>`（例: `aci.ui/index.html`、`aci.ui/img/knob.png`）。中身はファイルのバイト列そのまま |
| パス | 英数字と `.` `_` `-` `/` のみ。`..`・先頭 `/`・空要素は不可。重複したパスは無効 |
| サイズ | UI 全体で 8 MiB 以下 (MUST)、1 MiB 以下を推奨 (SHOULD)。DAW のプロジェクトにも保存されるため |
| 種類 | `index.html` があれば **HTML UI**、`ui.json` があれば **スキン**。両方あってよく、ホストは HTML を優先し、Web ビューが使えなければスキンにフォールバックする (SHOULD) |

UI はパラメータ（§4）を操作するだけで、音源の wasm とは直接やり取りしません。UI に不備があっても音源のロードは
妨げず、ホストは既定の UI を表示します (MUST)。

### 10.1 スキン（`ui.json`）

画像と配置を宣言するだけのネイティブ描画 UI。コードは実行されません。

```json
{
  "size": [520, 220],
  "background": "panel.svg",
  "color": "#e8dcc4",
  "accent": "#ffaa3c",
  "font": "font.ttf",
  "elements": [
    { "type": "knob",  "param": "Cutoff", "x": 40, "y": 60, "w": 80, "h": 80, "src": "knob.png", "frames": 64 },
    { "type": "label", "text": "CUTOFF", "x": 30, "y": 142, "w": 100, "h": 16, "size": 11, "bold": true },
    { "type": "value", "param": "Cutoff", "x": 30, "y": 160, "w": 100, "h": 16 }
  ]
}
```

| ルートのキー | 説明 |
|---|---|
| `size` | 必須。デザイン上の大きさ `[幅, 高さ]`（px）。ホストは縦横比を保って拡大縮小する |
| `background` | `"#rrggbb"` の色、または画像ファイル（`size` に引き伸ばす） |
| `color` / `accent` | 文字色 / コントロールの強調色の既定値 |
| `font` | TTF/OTF ファイル。全テキストに使う |
| `elements` | 必須。要素の配列。**配列の順に重ねて描画**する |

全要素に `type`, `x`, `y`, `w`, `h`（デザイン座標）が必須。`color` で色を上書きできる。`param` はパラメータの
`id`（整数）または `name`（大文字小文字を区別しない）。

| `type` | キー | 説明 |
|---|---|---|
| `image` | `src` | 画像を表示 |
| `label` | `text`, `size`, `bold`, `align` (`left`/`center`/`right`) | 固定テキスト |
| `value` | `param`, `size`, `bold`, `align` | パラメータの現在値（単位・ラベル付き） |
| `knob` | `param`, `src`, `frames`, `angle` | 縦ドラッグで操作。`src` + `frames` > 1 は**フィルムストリップ**（全コマを長辺方向に並べた 1 枚の画像）、`frames` 無しは画像を `angle`（`[開始, 終了]` 度、0 = 真上、時計回り、既定 `[-135, 135]`）の範囲で回転。`src` 無しはホストが描画 |
| `slider` | `param`, `src`（つまみ画像）, `track`（溝の画像） | `h > w` なら縦、それ以外は横 |
| `toggle` | `param`, `src`（2 コマのフィルムストリップ: off, on）, `text` | クリックで `min` ⇔ `max`。`text` があれば点灯ボタン、無ければスイッチ |
| `menu` | `param` | `steps` を持つパラメータのドロップダウン（`labels` を表示） |
| `buttons` | `param` | `steps` を持つパラメータの択一ボタン列 |

画像は PNG・JPEG・GIF・SVG（SVG はグラデーションを含む基本図形。フィルタ・CSS・埋め込みフォントは非対応）。
共通操作: ダブルクリックで既定値、Shift + ドラッグで微調整、ホイール。

### 10.2 HTML UI（`index.html`）

ホストは `index.html` を Web ビュー（Windows: WebView2、macOS: WKWebView、Linux: WebKitGTK）で表示し、
バンドル内のファイルを相対パスで配信します。

- **ネットワークには接続できない** (MUST)。外部 URL の読み込み・通信・他ページへの遷移はホストが遮断する。
  CSS・JS・画像・フォントはすべてバンドルに含める
- 推奨サイズを `<meta name="aci-size" content="720x300">` で宣言する（既定 720×360）。ホストはこの大きさ
  （入りきらなければ縮めた大きさ）のビューを用意する
- ホストはページのスクリプトより先に `window.aci` を定義する。ページが自分で読み込むライブラリは不要

| `window.aci` | 説明 |
|---|---|
| `params` | パラメータの配列 `{index, id, name, min, max, default, unit, scale, steps, labels, value}` |
| `instrument`, `slot` | メタデータ（`id`, `name`, `version`, `author`, `description`）とスロット ID |
| `param(ref)` | `id`・名前・パラメータオブジェクトからパラメータを引く |
| `get(ref)` / `set(ref, value)` | 実値の取得・設定（範囲と `steps` に丸められる） |
| `getNormalized(ref)` / `setNormalized(ref, n)` | 0–1 の正規化値（`scale: "log"` を考慮） |
| `toNormalized(ref, v)` / `fromNormalized(ref, n)` | 変換のみ |
| `format(ref, value?)` | 表示用文字列（`"2.50 kHz"`、`"10 ms"`、ラベル） |
| `onChange(fn)` | `fn(param, value)`: 値が変わるたび（UI 自身の `set`、オートメーション、API による変更）に呼ばれる。解除関数を返す |
| `note(key, velocity = 100, ms = 500)` | 試聴用にこのスロットでノートを鳴らす |
| `log(message)` | ホストのログに出力（未捕捉の例外も自動で送られる） |
| `bind(root?)` | 宣言的バインドを（再）適用 |

宣言的バインド: `data-aci-param="名前"` を付けた `<input type="range">`・`<select>`・`<input type="checkbox">` は
自動でパラメータと同期し、`data-aci-value="名前"` を付けた要素には現在値の文字列が入ります。

