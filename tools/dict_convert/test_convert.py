#!/usr/bin/env python3
"""Unit tests for the Chinese helpers in convert_jmdict.py: python3 tools/dict_convert/test_convert.py"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import convert_jmdict as conv  # noqa: E402


class PinyinMarks(unittest.TestCase):
    def test_tone_placement(self):
        self.assertEqual(conv.pinyin_to_marks("ni3 hao3"), "nǐ hǎo")
        self.assertEqual(conv.pinyin_to_marks("Zhong1 guo2"), "Zhōng guó")
        self.assertEqual(conv.pinyin_to_marks("lu:4"), "lǜ")
        self.assertEqual(conv.pinyin_to_marks("nu:3 er2"), "nǚ ér")
        self.assertEqual(conv.pinyin_to_marks("liu2"), "liú")  # mark on the last of iu
        self.assertEqual(conv.pinyin_to_marks("gui4"), "guì")  # and of ui
        self.assertEqual(conv.pinyin_to_marks("hou4"), "hòu")  # ou takes the o
        self.assertEqual(conv.pinyin_to_marks("xie4"), "xiè")  # e beats i

    def test_neutral_and_passthrough(self):
        self.assertEqual(conv.pinyin_to_marks("ma5"), "ma")
        self.assertEqual(conv.pinyin_to_marks("hua1 r5"), "huā r")
        self.assertEqual(conv.pinyin_to_marks("xx5"), "xx")
        self.assertEqual(conv.pinyin_to_marks("A B C"), "A B C")
        self.assertEqual(conv.pinyin_to_marks("m2"), "m")

    def test_bracketed_gloss(self):
        self.assertEqual(conv.prettify_cedict_gloss("CL:個|个[ge4]"), "CL:個|个[gè]")
        self.assertEqual(conv.prettify_cedict_gloss("see 你好[ni3 hao3]"), "see 你好[nǐ hǎo]")


class Zhuyin(unittest.TestCase):
    def test_syllables(self):
        cases = {
            "ni3": "ㄋㄧˇ", "hao3": "ㄏㄠˇ", "zhong1": "ㄓㄨㄥ", "guo2": "ㄍㄨㄛˊ", "lu:4": "ㄌㄩˋ",
            "ju2": "ㄐㄩˊ", "xue2": "ㄒㄩㄝˊ", "quan2": "ㄑㄩㄢˊ", "jun1": "ㄐㄩㄣ", "shi4": "ㄕˋ",
            "zi3": "ㄗˇ", "yi1": "ㄧ", "wu3": "ㄨˇ", "yu2": "ㄩˊ", "you3": "ㄧㄡˇ", "wei4": "ㄨㄟˋ",
            "ying1": "ㄧㄥ", "yong4": "ㄩㄥˋ", "er2": "ㄦˊ", "a1": "ㄚ", "ma5": "˙ㄇㄚ", "r5": "˙ㄦ",
            "liu2": "ㄌㄧㄡˊ", "gui4": "ㄍㄨㄟˋ", "lun4": "ㄌㄨㄣˋ", "feng1": "ㄈㄥ", "weng1": "ㄨㄥ",
        }
        for pinyin, expected in cases.items():
            with self.subTest(pinyin=pinyin):
                self.assertEqual(conv.pinyin_syllable_to_zhuyin(pinyin), expected)

    def test_unknown_passthrough(self):
        self.assertEqual(conv.pinyin_syllable_to_zhuyin("xx5"), "xx5")
        self.assertEqual(conv.pinyin_syllable_to_zhuyin("·"), "·")


class Frequency(unittest.TestCase):
    def test_rank_scale(self):
        self.assertEqual(conv.rank_to_priority(1), 255)
        self.assertGreater(conv.rank_to_priority(10), conv.rank_to_priority(100))
        self.assertGreater(conv.rank_to_priority(100000), conv.UNRANKED_PRIORITY)

    def test_count_list_and_graded_list(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            jieba = os.path.join(d, "dict.txt")
            with open(jieba, "w", encoding="utf-8") as f:
                f.write("你好 5000 l\n这 90000 r\n中国 80000 ns\n")
            p = conv.load_frequency(jieba)
            self.assertEqual(p["这"], 255)
            self.assertGreater(p["中国"], p["你好"])

            hsk = os.path.join(d, "hsk30.csv")
            with open(hsk, "w", encoding="utf-8") as f:
                f.write("ID,Simplified,Traditional,Pinyin,Level\n1,这,這,zhè,1\n2,说话,說話,shuō huà,2\n")
            p = conv.load_frequency(hsk)
            self.assertEqual(p["这"], 255)  # row order, not the level column
            self.assertGreater(p["这"], p["说话"])


class Cedict(unittest.TestCase):
    def test_definition(self):
        text = conv.format_definition_cedict("說話", "说话", "shuo1 hua4", ["to speak", "to say"], zhuyin=True)
        self.assertEqual(text, "【shuō huà ㄕㄨㄛ ㄏㄨㄚˋ】\n→ 說話 / 说话\n• to speak\n\n• to say\n")
        text = conv.format_definition_cedict("你好", "你好", "ni3 hao3", ["hello"], zhuyin=False)
        self.assertEqual(text, "【nǐ hǎo】\n• hello\n")




class CedictExtras(unittest.TestCase):
    def test_proper_noun_detection(self):
        self.assertTrue(conv.is_proper_noun_pinyin("Zhong1 guo2"))
        self.assertTrue(conv.is_proper_noun_pinyin("Bei3 jing1"))
        self.assertFalse(conv.is_proper_noun_pinyin("ni3 hao3"))
        self.assertFalse(conv.is_proper_noun_pinyin("xx5"))

    def test_canto_line_and_level_tag(self):
        m = conv._CEDICT_LINE_RE.match("你好 你好 [ni3 hao3] {nei5 hou2} /hello/")
        self.assertIsNotNone(m)
        self.assertEqual(m.group(4), "nei5 hou2")
        text = conv.format_definition_cedict("你好", "你好", "ni3 hao3", ["hello"], False, "nei5 hou2", "HSK 1")
        self.assertEqual(text, "【nǐ hǎo · nei5 hou2】\n[HSK 1]\n• hello\n")

    def test_levels_and_tsv(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            hsk = os.path.join(d, "hsk30.csv")
            with open(hsk, "w", encoding="utf-8") as f:
                f.write("ID,Simplified,Traditional,Pinyin,POS,Level\n1,这,這,zhè,r,1\n2,说话,說話,shuō huà,v,7-9\n")
            levels = conv.load_levels(hsk, "HSK")
            self.assertEqual(levels["这"], "HSK 1")
            self.assertEqual(levels["這"], "HSK 1")
            self.assertEqual(levels["說話"], "HSK 7-9")
            tsv = os.path.join(d, "grammar.tsv")
            with open(tsv, "w", encoding="utf-8") as f:
                f.write("# comment\n把\tdisposal construction\\nS + 把 + O + V\tHSK 3\n\n")
            records = conv.convert_tsv(tsv)
            self.assertEqual(len(records), 1)
            self.assertEqual(records[0][0], "把".encode("utf-8"))
            self.assertEqual(records[0][1].decode("utf-8"), "disposal construction\nS + 把 + O + V\nHSK 3")


class Examples(unittest.TestCase):
    def test_pairs_attach_to_multi_character_words(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            pairs = os.path.join(d, "pairs.tsv")
            with open(pairs, "w", encoding="utf-8") as f:
                f.write("1\t我在中国说话。\t2\tI speak in China.\n")
                f.write("3\t" + "很" * 50 + "\t4\ttoo long\n")
                f.write("5\t你好。\t6\tHello.\n")
            loaded = conv.load_sentence_pairs(pairs)
            self.assertEqual(len(loaded), 2)
            forms = {"中国": 0, "中國": 0, "说话": 1, "說話": 1, "的": 2, "你好": 3}
            ex = conv.attach_examples(loaded, forms, 4)
            self.assertEqual(ex[0], [("我在中国说话。", "I speak in China.")])
            self.assertEqual(ex[1], [("我在中国说话。", "I speak in China.")])
            self.assertEqual(ex[2], [])  # single characters get none
            self.assertEqual(ex[3], [("你好。", "Hello.")])
            text = conv.format_definition_cedict("中國", "中国", "Zhong1 guo2", ["China"], False, examples=ex[0])
            self.assertTrue(text.endswith("China\n\n我在中国说话。\nI speak in China."), text)


class ScriptFilters(unittest.TestCase):
    def test_sentence_script(self):
        self.assertEqual(conv.sentence_script("这是说话"), "simplified")
        self.assertEqual(conv.sentence_script("這是說話"), "traditional")
        self.assertEqual(conv.sentence_script("人山人海"), "any")

    def test_level_from_row_id(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            tocfl = os.path.join(d, "tocfl.csv")
            with open(tocfl, "w", encoding="utf-8") as f:
                f.write("ID,Traditional,Simplified,Pinyin,POS,Variants\nL0-1001,我,我,wǒ,N,\nL3-0012,說話,说话,shuō huà,V,\n")
            levels = conv.load_levels(tocfl, "TOCFL")
            self.assertEqual(levels["我"], "TOCFL Novice")
            self.assertEqual(levels["说话"], "TOCFL 3")


class ListParsing(unittest.TestCase):
    def test_variant_cells_and_id_columns(self):
        import tempfile, os
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "levels.csv")
            with open(path, "w", encoding="utf-8") as f:
                f.write("ID,Traditional,Simplified,Level\n2,你/妳,你,1\n3,爸爸|爸,爸爸,7-9\n")
            levels = conv.load_levels(path, "HSK")
            self.assertEqual(levels["妳"], "HSK 1")
            self.assertEqual(levels["爸"], "HSK 7-9")
            # The level column, not a row id or a web number, and never a count.
            priorities = conv.load_frequency(path)
            self.assertGreater(priorities["你"], priorities["爸爸"])

    def test_twin_rank_takes_the_larger(self):
        records = [("說".encode(), b"", 100, 0), ("说".encode(), b"", 100, 0), ("貓".encode(), b"", 100, 0)]
        out = conv.apply_frequency(records, {"说": 219, "說": 102, "猫": 150}, {"說": "说", "说": "說", "貓": "猫"})
        self.assertEqual([p for _, _, p, _ in out], [219, 219, 150])


if __name__ == "__main__":
    unittest.main()
