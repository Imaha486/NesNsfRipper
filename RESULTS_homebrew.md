# フリー配布ホームブリューROM 68本の解析結果 (nsfrip_cli v1.4, デフォルト設定)

出典: GitHub `retrobrews/nes-games`（作者が無料配布しているホームブリュー作品集）。「ファミコン音楽V2」の DB に無いタイトル群の代表として使用。

曲 = 曲候補数（妥当と判定されたもの）、効果音 = 効果音候補数、NSF検証 = 生成したNSFを内蔵プレイヤーで再生しサウンドレジスタ書き込み列がハーネス実行と一致したトラック数/出力トラック数。v1.3 までは効果音を分類していなかったため、旧版の「曲」欄は曲+効果音の合計に相当する。

| ROM | マッパー | 曲 | 効果音 | NSF検証 (一致/出力) | PLAY | 備考 |
|---|---|---|---|---|---|---|
| 31in1realgame-multicart | mapper 113) | 8 | 16 | 24/24 | $87E6 |  |
| 3in12ppak | mapper 0 (NROM) | 16 | 176 | 192/192 | $DBE2 |  |
| ambushed | mapper 0 (NROM) | 0 | 0 | - | $FE76 (interrupt handler) | SEARCH FAILED: no candidates |
| assimilate | mapper 1 (MMC1) | 38 | 16 | 54/54 | $FD3F |  |
| babelblox | mapper 30 (UNROM 512) | 123 | 22 | 145/145 | $8142 |  |
| blackboxchallenge | mapper 30 (UNROM 512) | 0 | 119 | 119/119 | $8242 |  |
| blaster | mapper 0 (NROM) | 22 | 10 | 31/32 | $D55F |  |
| bombarray | mapper 0 (NROM) | 0 | 0 | - |  | ANALYZE FAILED: 毎フレーム呼ばれるサウンド更� |
| bootee | mapper 0 (NROM) | 2 | 3 | 5/5 | $8795 |  |
| bronyblaster | mapper 1 (MMC1) | 0 | 0 | - |  | ANALYZE FAILED: ゲーム実行中にサウンドレジス� |
| cheril-the-goddess | mapper 2 (UxROM) | 5 | 7 | 12/12 | $C8FB |  |
| cl1k | mapper 0 (NROM) | 27 | 0 | 27/27 | $FE64 (interrupt handler) |  |
| croom | mapper 0 (NROM) | 1 | 30 | 31/31 | $D501 |  |
| dabg | mapper 0 (NROM) | 6 | 13 | 19/19 | $F633 |  |
| debrisdodger | mapper 0 (NROM) | 0 | 0 | - | $FE5B (interrupt handler) |  |
| driar | mapper 1 (MMC1) | 4 | 7 | 11/11 | $B742 |  |
| falling | mapper 0 (NROM) | 48 | 19 | 66/67 | $80F1 |  |
| fff | mapper 2 (UxROM) | 0 | 0 | - |  | ANALYZE FAILED: 毎フレーム呼ばれるサウンド更� |
| filthykitchen | mapper 28 (Action 53) | 4 | 13 | 17/17 | $C6DA |  |
| flappybird | mapper 0 (NROM) | 2 | 2 | 2/4 | $8860 |  |
| flappyblock | mapper 0 (NROM) | 0 | 0 | - |  | ANALYZE FAILED: ゲーム実行中にサウンドレジス� |
| flappyjack | mapper 0 (NROM) | 3 | 0 | 3/3 | $A463 |  |
| forpoints | mapper 0 (NROM) | 0 | 0 | - |  | ANALYZE FAILED: ゲーム実行中にサウンドレジス� |
| gsm | mapper 0 (NROM) | 0 | 0 | - |  | ANALYZE FAILED: ゲーム実行中にサウンドレジス� |
| indivisibleonnes | mapper 4 (MMC3) | 8 | 43 | 25/30 | $81A5 | スタブバンクが4KBを超えました (4264 bytes: RAM� |
| invaders | mapper 0 (NROM) | 0 | 0 | - | $9AD6 |  |
| jetpaco | mapper 0 (NROM) | 3 | 10 | 13/13 | $8755 |  |
| kyff | mapper 0 (NROM) | 0 | 118 | 118/118 | $B084 |  |
| lala | mapper 66 (GxROM) | 12 | 24 | 36/36 | $86E3 | WARN: パッチ不可のマッパー書き込み: PC=$8A26 - |
| lightshields | mapper 0 (NROM) | 1 | 0 | 1/1 | $CBB1 |  |
| lunarlimit | mapper 0 (NROM) | 20 | 13 | 33/33 | $A8C7 |  |
| madwizard | mapper 0 (NROM) | 17 | 20 | 37/37 | $F66E |  |
| mashymashy | mapper 0 (NROM) | 0 | 0 | - |  | ANALYZE FAILED: ゲーム実行中にサウンドレジス� |
| megamountain | mapper 0 (NROM) | 15 | 4 | 10/19 | $9D74 |  |
| memory | mapper 0 (NROM) | 0 | 0 | - |  | ANALYZE FAILED: ゲーム実行中にサウンドレジス� |
| midnightjogger | mapper 0 (NROM) | 0 | 0 | - |  | ANALYZE FAILED: 毎フレーム呼ばれるサウンド更� |
| miedow | mapper 3 (CNROM) | 8 | 5 | 13/13 | $87B3 |  |
| mineshaft | mapper 0 (NROM) | 1 | 2 | 3/3 | $8883 |  |
| mouser2 | mapper 0 (NROM) | 14 | 1 | 2/15 | $AB03 |  |
| nesertbus | mapper 0 (NROM) | 7 | 9 | 16/16 | $C0D5 |  |
| ninjamuncher | mapper 0 (NROM) | 1 | 0 | 1/1 | $FEB8 (interrupt handler) |  |
| nomolos | mapper 2 (UxROM) | 224 | 85 | 0/309 | $F0D4 | WARN: パッチ不可のマッパー書き込み: PC=$ED08 - |
| nopoints | mapper 0 (NROM) | 0 | 0 | - |  | ANALYZE FAILED: 毎フレーム呼ばれるサウンド更� |
| obstacletrek | mapper 0 (NROM) | 0 | 0 | - |  | ANALYZE FAILED: 毎フレーム呼ばれるサウンド更� |
| owlia | mapper 2 (UxROM) | 74 | 3 | 76/77 | $866E | WARN: パッチ不可のマッパー書き込み: PC=$BDC0 - |
| pegs | mapper 0 (NROM) | 84 | 5 | 74/80 | $C5FD (interrupt handler) | スタブバンクが4KBを超えました (4237 bytes: RAM� |
| pong1k | mapper 0 (NROM) | 0 | 0 | - | $FE4A (interrupt handler) | SEARCH FAILED: no candidates |
| pong1k2p | mapper 0 (NROM) | 0 | 0 | - | $FE4A (interrupt handler) | SEARCH FAILED: no candidates |
| ralph4 | mapper 0 (NROM) | 3 | 2 | 3/5 | $CCAB | WARN: Music 03: ハーネス実行失敗 |
| rhde | mapper 0 (NROM) | 1 | 10 | 11/11 | $B6B8 |  |
| riseofamondus | mapper 2 (UxROM) | 164 | 33 | 197/197 | $B52D |  |
| roboninjaclimb | mapper 3 (CNROM) | 1 | 0 | 1/1 | $A366 |  |
| rpsls | mapper 0 (NROM) | 6 | 7 | 13/13 | $9C03 |  |
| sgthelmet | mapper 0 (NROM) | 2 | 10 | 12/12 | $8755 |  |
| simonesays | mapper 0 (NROM) | 0 | 4 | 4/4 | $FDC7 (interrupt handler) |  |
| snailmaze | mapper 0 (NROM) | 22 | 87 | 109/109 | $AF26 |  |
| spaceymcracey | mapper 0 (NROM) | 6 | 3 | 9/9 | $8D95 |  |
| starevil | mapper 0 (NROM) | 1 | 0 | 1/1 | $FA33 |  |
| superpakpak | mapper 2 (UxROM) | 5 | 0 | 5/5 | $8A64 |  |
| superuwol | mapper 0 (NROM) | 10 | 59 | 69/69 | $87ED |  |
| thatswhack | mapper 0 (NROM) | 0 | 0 | - | $FECE (interrupt handler) | SEARCH FAILED: no candidates |
| theinvasion | mapper 0 (NROM) | 4 | 0 | 4/4 | $FEF4 (interrupt handler) |  |
| themadwizard | mapper 2 (UxROM) | 10 | 18 | 28/28 | $F619 |  |
| theonewiththewalls | mapper 0 (NROM) | 0 | 0 | - |  | ANALYZE FAILED: 毎フレーム呼ばれるサウンド更� |
| thwaite | mapper 0 (NROM) | 3 | 10 | 13/13 | $E12E |  |
| tictactwop | mapper 0 (NROM) | 0 | 0 | - |  | ANALYZE FAILED: ゲーム実行中にサウンドレジス� |
| tictacxo | mapper 0 (NROM) | 10 | 2 | 12/12 | $8977 |  |
| tigerjenny | mapper 0 (NROM) | 3 | 4 | 7/7 | $8785 |  |

合計: NSF生成 49/68 本、全トラック一致 39 本、トラック一致 1714/2063

## v1.3 → v1.4 で変わった主なタイトル

- cheril-the-goddess 3→5曲+7効果音、lala 0→12曲、superuwol 4→10曲、31in1realgame-multicart 1→8曲: Mojon Twins 系エンジン。起動直後のスナップショットでは曲 API が効かないため、ドライバ初期化後（音が鳴っている瞬間）のスナップショットからの探索で曲 API（`JSR $xxxx (A=曲番号)`）を発見。
- owlia 2→74曲: PLAY がシャドウレジスタの転送だけで、曲の計算はメインループ側の `$8040` が行う構成（インラインエンジン）。PLAY 単体の出力が凍結していることを検出し、エンジンルーチンを PLAY に組み込んだ。
- rhde 0→1曲: 自動プレイがタイトル画面から進まず FamiTone の music_play が一度も呼ばれない。ROM を走査してドライバ近傍の未使用サブルーチン（`$B74B`）を INIT 候補に加える「静的候補」で発見。
- assimilate 0→38曲、fff など: NES コアの vblank 処理の修正で起動するようになった（`BIT $2002` でフラグをポーリングするメインループが、NMI ハンドラの `$2002` 読み出しに先を越されて永久に待っていた。実機ではフラグ設定直後の数サイクルで読めるため動く。あわせて、1 スキャンラインごとに約 1 サイクルずつ長くなっていたフレーム長も修正）。
- ambushed 49→0、blackboxchallenge 693→0曲、kyff 538→0曲、rhde 74→1曲 など「曲」が激減したもの: 旧版の数字は OAM 領域やチャンネルレジスタをメールボックス扱いした誤検出（ほぼ全値で「鳴る」変数）で、ambushed はゲーム自体に曲が無い。v1.4 では 160 値以上で鳴る変数が 3 つ以上ある場合にレジスタ的変数として降格する。
- nomolos: ドライバが `INC $EEEE` 形式（RMW 命令）でバンク切替しており、現状はパッチ不可（検証 NG）。bombarray / bronyblaster / fff / gsm など: 自動プレイ中にサウンド出力が無い（曲が無いか、到達できない）。
