#!/usr/bin/env python3
"""
Generate a vertical Japanese test EPUB for word lookup of numbers + counters.

Each sentence holds a number written in digits followed by a counter. Look the
number up and check that the panel shows the counter sense with the whole
reading given in brackets after the sentence (６月 -> ろくがつ, not つき).
Half-width and full-width digits are both used. The last chapter holds
controls: counters the reading table does not cover, and kanji numerals, which
the dictionary already reads as whole words.
"""

import zipfile
from pathlib import Path

OUTPUT_DIR = Path(__file__).parent.parent / "test" / "epubs"
OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
BOOK_ID = "test-counter-reading-001"

CHAPTERS = [
    ("月", [
        ("1月に雪が降った。", "いちがつ"),
        ("４月になると桜が咲く。", "しがつ"),
        ("６月は雨が多い。", "ろくがつ"),
        ("7月の海は混む。", "しちがつ"),
        ("９月に学校が始まる。", "くがつ"),
        ("12月は忙しい。", "じゅうにがつ"),
    ]),
    ("日", [
        ("1日に引っ越した。", "ついたち・いちにち"),
        ("２日の朝に着いた。", "ふつか"),
        ("3日待った。", "みっか"),
        ("４日目に晴れた。", "よっかめ"),
        ("5日後に届いた。", "いつか"),
        ("６日に出発する。", "むいか"),
        ("７日間続いた。", "なのかかん"),
        ("9日の午後。", "ここのか"),
        ("8日の夜だった。", "ようか"),
        ("１０日で終わる。", "とおか"),
        ("14日に会おう。", "じゅうよっか"),
        ("２０日が締め切りだ。", "はつか"),
        ("21日に死亡した。", "にじゅういちにち"),
        ("２４日は休みだ。", "にじゅうよっか"),
        ("6月21日の出来事だ。", "ろくがつ・にじゅういちにち"),
    ]),
    ("年・時・分・時間", [
        ("1582年の事件だ。", "せんごひゃくはちじゅうにねん"),
        ("１９６４年に建った。", "せんきゅうひゃくろくじゅうよねん"),
        ("4年かかった。", "よねん"),
        ("９時に起きた。", "くじ"),
        ("4時に会う。", "よじ"),
        ("７時のニュース。", "しちじ"),
        ("10時40分に出た。", "じゅうじ・よんじゅっぷん"),
        ("３分待って。", "さんぷん"),
        ("5分で戻る。", "ごふん"),
        ("６分かかる。", "ろっぷん"),
        ("２時間歩いた。", "にじかん"),
        ("4時間眠った。", "よじかん"),
    ]),
    ("人・歳", [
        ("1人で来た。", "ひとり"),
        ("２人で話した。", "ふたり"),
        ("4人が残った。", "よにん"),
        ("５人の若者が亡くなった。", "ごにん"),
        ("１歳の子供だ。", "いっさい"),
        ("20歳になった。", "はたち"),
        ("８歳の頃だ。", "はっさい"),
        ("30才の男。", "さんじゅっさい"),
    ]),
    ("回・階・ヶ月", [
        ("1回だけ見た。", "いっかい"),
        ("６回目の挑戦。", "ろっかいめ"),
        ("11回も電話した。", "じゅういっかい"),
        ("3階に住んでいる。", "さんがい"),
        ("８階の部屋。", "はっかい"),
        ("２ヶ月が過ぎた。", "にかげつ"),
        ("6ヶ月後に戻った。", "ろっかげつ"),
        ("１か月待った。", "いっかげつ"),
        ("10カ月の旅。", "じゅっかげつ"),
    ]),
    ("個・冊・本・枚・円", [
        ("1個ください。", "いっこ"),
        ("６個買った。", "ろっこ"),
        ("8冊読んだ。", "はっさつ"),
        ("１冊の本。", "いっさつ"),
        ("1本の木。", "いっぽん"),
        ("３本の線。", "さんぼん"),
        ("30本の鉛筆。", "さんじゅっぽん"),
        ("４枚の紙。", "よんまい"),
        ("300円の本。", "さんびゃくえん"),
        ("４円足りない。", "よえん"),
        ("８０００円払った。", "はっせんえん"),
        ("25000円の時計。", "にまんごせんえん"),
    ]),
    ("匹・杯・頭・羽ほか", [
        ("1匹の犬。", "いっぴき"),
        ("3匹の猫がいた。", "さんびき"),
        ("６匹の魚。", "ろっぴき"),
        ("1杯の水。", "いっぱい"),
        ("３杯飲んだ。", "さんばい"),
        ("2頭の馬。", "にとう"),
        ("８頭の牛。", "はっとう"),
        ("２台の車。", "にだい"),
        ("４台のバス。", "よんだい"),
    ]),
    ("家・件・足・着・通", [
        ("3軒の家。", "さんげん"),
        ("１軒目。", "いっけんめ"),
        ("1件のメール。", "いっけん"),
        ("６件の事件。", "ろっけん"),
        ("3足の靴。", "さんぞく"),
        ("１着のスーツ。", "いっちゃく"),
        ("1通の手紙。", "いっつう"),
        ("２通届いた。", "につう"),
    ]),
    ("歩・泊・発・曲・品・点・社", [
        ("1歩進んだ。", "いっぽ"),
        ("３歩下がる。", "さんぽ"),
        ("2泊した。", "にはく"),
        ("３泊の旅。", "さんぱく"),
        ("1発撃った。", "いっぱつ"),
        ("１曲歌った。", "いっきょく"),
        ("8品の料理。", "はっぴん"),
        ("１０点取った。", "じゅってん"),
        ("1社に受かった。", "いっしゃ"),
    ]),
    ("週・度・倍・秒・番・号・名・年生", [
        ("1週間休んだ。", "いっしゅうかん"),
        ("２週目。", "にしゅうめ"),
        ("3度目の正直。", "さんどめ"),
        ("４度の熱。", "よんど"),
        ("2倍になった。", "にばい"),
        ("４秒待つ。", "よんびょう"),
        ("1番になった。", "いちばん"),
        ("３号室。", "さんごう"),
        ("５名様。", "ごめい"),
        ("4年生になった。", "よねんせい"),
        ("１０年間住んだ。", "じゅうねんかん"),
        ("４時半に起きた。", "よじはん"),
    ]),
    ("つ・単位", [
        ("3つ選んだ。", "みっつ"),
        ("７つの海。", "ななつ"),
        ("１０センチ伸びた。", "じゅっセンチ"),
        ("5キロ走った。", "ごキロ"),
        ("２メートルの壁。", "にメートル"),
        ("100グラムの肉。", "ひゃくグラム"),
        ("１ページ目。", "いっページめ"),
        ("10パーセント引き。", "じゅっパーセント"),
    ]),
    ("百・千・万・億", [
        ("100本の花。", "ひゃっぽん"),
        ("３００本の木。", "さんびゃっぽん"),
        ("1000本の針。", "せんぼん"),
        ("３０００本の線。", "さんぜんぼん"),
        ("100回言った。", "ひゃっかい"),
        ("5万円の時計。", "ごまんえん"),
        ("３億円の家。", "さんおくえん"),
        ("1兆円の予算。", "いっちょうえん"),
        ("３千円払った。", "さんぜんえん"),
        ("１億２０００万人。", "いちおくにせんまんにん"),
    ]),
    ("目", [
        ("3日目に晴れた。", "みっかめ"),
        ("１日目は雨。", "いちにちめ"),
        ("2回目の挑戦。", "にかいめ"),
        ("３人目の客。", "さんにんめ"),
        ("1年目の春。", "いちねんめ"),
        ("５番目の席。", "ごばんめ"),
        ("２ページ目。", "にページめ"),
    ]),
    ("羽・隻・和語の助数詞", [
        ("1羽の鳥。", "いちわ"),
        ("3羽の鶴。", "さんば"),
        ("６羽のうさぎ。", "ろっぱ"),
        ("1隻の船。", "いっせき"),
        ("1箱のりんご。", "ひとはこ"),
        ("２束の花。", "ふたたば"),
        ("1口食べた。", "ひとくち"),
        ("２組の夫婦。", "ふたくみ"),
        ("1晩泊まった。", "ひとばん"),
        ("１袋の米。", "ひとふくろ"),
        ("２切れのケーキ。", "ふたきれ"),
        ("1皿のカレー。", "ひとさら"),
        ("２部屋のアパート。", "ふたへや"),
        ("1言だけ。", "ひとこと"),
        ("１粒の涙。", "ひとつぶ"),
    ]),
    ("建物・場所・文字", [
        ("1棟のマンション。", "いっとう"),
        ("６畳の部屋。", "ろくじょう"),
        ("4畳半。", "よじょう"),
        ("20坪の土地。", "にじゅうつぼ"),
        ("３段の階段。", "さんだん"),
        ("２列に並ぶ。", "にれつ"),
        ("3行目を読む。", "さんぎょうめ"),
        ("１００文字以内。", "ひゃくもじ"),
        ("４字熟語。", "よじ"),
        ("５問解いた。", "ごもん"),
        ("1席空いている。", "いっせき"),
        ("１票差。", "いっぴょう"),
        ("2人前の料理。", "ににんまえ"),
    ]),
    ("時の単位", [
        ("1世紀前。", "いっせいき"),
        ("１０周年。", "じゅっしゅうねん"),
        ("２０代の頃。", "にじゅうだい"),
        ("１学期が終わる。", "いちがっき"),
        ("3時限目。", "さんじげんめ"),
        ("10分間休んだ。", "じゅっぷんかん"),
    ]),
    ("ローマ字の単位", [
        ("5kmを走った。", "ごキロメートル"),
        ("３kgの米。", "さんキログラム"),
        ("10cm伸びた。", "じゅっセンチメートル"),
        ("５mmの差。", "ごミリメートル"),
        ("100mの距離。", "ひゃくメートル"),
        ("５００gの肉。", "ごひゃくグラム"),
        ("10%引き。", "じゅっパーセント"),
        ("３０％が賛成。", "さんじゅっパーセント"),
        ("30℃を超えた。", "さんじゅうど"),
        ("2トンの車。", "にトン"),
        ("１００ドル札。", "ひゃくドル"),
        ("２リットルの水。", "にリットル"),
    ]),
    ("対照", [
        ("3体の人形。", "体は表にない：辞書の読みのまま"),
        ("２脚の椅子。", "脚は表にない：辞書の読みのまま"),
        ("六月に咲く。", "漢数字：ろくがつ（辞書の見出し）"),
        ("二人で歩いた。", "漢数字：ふたり（辞書の見出し）"),
        ("月が明るい。", "数字なし：つき"),
        ("日が沈む。", "数字なし：ひ"),
    ]),
]


def chapter_xhtml(title, lines):
    body = "\n".join(f"<p>{s}（{r}）</p>" for s, r in lines)
    return f'''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="ja" lang="ja">
<head>
  <title>{title}</title>
  <link rel="stylesheet" type="text/css" href="style.css"/>
</head>
<body>
  <h1>{title}</h1>
{body}
</body>
</html>'''


def create_epub(path):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as epub:
        epub.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        epub.writestr("META-INF/container.xml", '''<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>''')
        epub.writestr("OEBPS/style.css", "html { writing-mode: vertical-rl; -epub-writing-mode: vertical-rl; }\n")
        manifest, spine, nav = [], [], []
        for i, (title, lines) in enumerate(CHAPTERS):
            cid = f"chapter{i}"
            epub.writestr(f"OEBPS/{cid}.xhtml", chapter_xhtml(title, lines))
            manifest.append(f'<item id="{cid}" href="{cid}.xhtml" media-type="application/xhtml+xml"/>')
            spine.append(f'<itemref idref="{cid}"/>')
            nav.append(f'''<navPoint id="nav{i + 1}" playOrder="{i + 1}">
      <navLabel><text>{title}</text></navLabel><content src="{cid}.xhtml"/></navPoint>''')
        epub.writestr("OEBPS/content.opf", f'''<?xml version="1.0" encoding="UTF-8"?>
<package version="2.0" xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>助数詞テスト</dc:title>
    <dc:creator>CrossPoint Test Generator</dc:creator>
    <dc:language>ja</dc:language>
    <dc:identifier id="bookid">{BOOK_ID}</dc:identifier>
  </metadata>
  <manifest>
    {"".join(manifest)}
    <item id="css" href="style.css" media-type="text/css"/>
    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
  </manifest>
  <spine toc="ncx" page-progression-direction="rtl">
    {"".join(spine)}
  </spine>
</package>''')
        epub.writestr("OEBPS/toc.ncx", f'''<?xml version="1.0" encoding="UTF-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
  <head><meta name="dtb:uid" content="{BOOK_ID}"/></head>
  <docTitle><text>助数詞テスト</text></docTitle>
  <navMap>
    {"".join(nav)}
  </navMap>
</ncx>''')


if __name__ == "__main__":
    out = OUTPUT_DIR / "test_counter_reading.epub"
    create_epub(out)
    print(f"Created: {out}")
