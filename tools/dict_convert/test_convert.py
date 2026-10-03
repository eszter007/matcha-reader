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
        self.assertEqual(text, "【shuō huà ㄕㄨㄛ ㄏㄨㄚˋ】\n說話 / 说话\n1. to speak\n2. to say")
        text = conv.format_definition_cedict("你好", "你好", "ni3 hao3", ["hello"], zhuyin=False)
        self.assertEqual(text, "【nǐ hǎo】\nhello")


if __name__ == "__main__":
    unittest.main()
