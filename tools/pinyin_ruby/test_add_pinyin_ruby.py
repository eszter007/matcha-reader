import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import add_pinyin_ruby as tool  # noqa: E402

WORDS = {"石": ["dan4"], "石頭": ["shi2", "tou5"], "一": ["yi1"]}
READINGS = {"石": {"dan4", "shi2"}, "頭": {"tou2", "tou5"}, "一": {"yi1"}, "塊": {"kuai4"}}


class ContextualReadings(unittest.TestCase):
    def test_a_listed_reading_replaces_the_dictionary_one(self):
        ctx = tool.contextual_readings(["此石"], READINGS, lambda batch: [["ci3", "shi2"]])
        self.assertEqual(ctx, {"此石": {1: "shi2"}})  # 此 is not in READINGS: left to the dictionary
        out = tool.annotate_xhtml("<p>此石</p>", WORDS, set(), False, ctx)
        self.assertIn("<ruby>石<rt>shí</rt></ruby>", out)

    def test_an_unlisted_reading_is_refused(self):
        ctx = tool.contextual_readings(["石"], READINGS, lambda batch: [["hua1"]])
        out = tool.annotate_xhtml("<p>石</p>", WORDS, set(), False, ctx)
        self.assertIn("<rt>dàn</rt>", out)

    def test_a_miscounted_answer_drops_the_passage(self):
        ctx = tool.contextual_readings(["一石"], READINGS, lambda batch: [["yi1"]])
        self.assertEqual(ctx, {})

    def test_no_answer_keeps_the_dictionary(self):
        ctx = tool.contextual_readings(["石"], READINGS, lambda batch: None)
        self.assertEqual(tool.annotate_xhtml("<p>石</p>", WORDS, set(), False, ctx),
                         tool.annotate_xhtml("<p>石</p>", WORDS, set(), False))

    def test_a_character_without_a_dictionary_word_takes_the_model_reading(self):
        ctx = tool.contextual_readings(["塊"], READINGS, lambda batch: [["kuai4"]])
        self.assertIn("<ruby>塊<rt>kuài</rt></ruby>", tool.annotate_xhtml("<p>塊</p>", WORDS, set(), False, ctx))

    def test_a_long_passage_is_asked_sentence_by_sentence(self):
        asked = []

        def ask(batch):
            asked.extend(batch)
            return [["yi1"] if s.startswith("一") else ["shi2"] for s in batch]

        ctx = tool.contextual_readings(["一。石。"], READINGS, ask)
        self.assertEqual(asked, ["一。", "石。"])
        self.assertEqual(ctx, {"一。石。": {0: "yi1", 2: "shi2"}})

    def test_spellings_are_normalised(self):
        self.assertEqual(tool.normalize_syllable("Lü4"), "lu:4")
        self.assertEqual(tool.normalize_syllable("de"), "de5")

    def test_markup_and_existing_ruby_are_untouched(self):
        doc = '<p title="石 > 一">石<ruby>一<rt>x</rt></ruby></p>'
        out = tool.annotate_xhtml(doc, WORDS, set(), False)
        self.assertIn('<p title="石 > 一">', out)
        self.assertIn("<ruby>一<rt>x</rt></ruby>", out)


class SpokenReadings(unittest.TestCase):
    WORDS = {"一": ["yi1"], "个": ["ge4"], "不": ["bu4"], "是": ["shi4"], "天": ["tian1"], "第": ["di4"],
             "哪儿": ["na3", "r5"], "慢": ["man4"], "地": ["di4"], "走": ["zou3"], "了": ["le5"],
             "一月": ["yi1", "yue4"], "星期一": ["xing1", "qi1", "yi1"], "统一": ["tong3", "yi1"],
             "行动": ["xing2", "dong4"], "人人": ["ren2", "ren2"], "地位": ["di4", "wei4"]}

    def rt(self, text, **kw):
        import re
        return re.findall(r"<ruby>(.*?)<rt>(.*?)</rt></ruby>", tool.annotate_xhtml(f"<p>{text}</p>", self.WORDS, set(), False, **kw))

    def test_yi_and_bu_change_tone(self):
        self.assertEqual(self.rt("一个")[0], ("一", "yí"))
        self.assertEqual(self.rt("一天")[0], ("一", "yì"))
        self.assertEqual(self.rt("不是")[0], ("不", "bú"))
        self.assertEqual(self.rt("第一")[1], ("一", "yī"))
        self.assertEqual(self.rt("一个", citation=True)[0], ("一", "yī"))

    def test_erhua_joins_the_syllable_before(self):
        self.assertEqual(self.rt("哪儿"), [("哪儿", "nǎr")])

    def test_de_after_a_doubled_adjective(self):
        self.assertEqual(self.rt("慢慢地走")[2], ("地", "de"))

    def test_yi_keeps_its_tone_in_fixed_words(self):
        self.assertEqual(self.rt("一月")[0], ("一", "yī"))
        self.assertEqual(self.rt("星期一是")[2], ("一", "yī"))
        self.assertEqual(self.rt("统一行动")[1], ("一", "yī"))

    def test_de_only_for_a_standalone_di(self):
        self.assertEqual(self.rt("人人地位")[2], ("地", "dì"))

    def test_context_crosses_inline_tags(self):
        import re
        def rt(doc):
            return re.findall(r"<ruby>(.*?)<rt>(.*?)</rt></ruby>", tool.annotate_xhtml(doc, self.WORDS, set(), False))
        self.assertEqual(rt("<p>不<em>是</em></p>")[0], ("不", "bú"))
        self.assertEqual(rt("<p>第<em>一天</em></p>")[1], ("一", "yī"))
        self.assertEqual(rt("<p>一<em>个</em></p>")[0], ("一", "yí"))
        self.assertEqual(rt("<p>不</p><p>是</p>")[0], ("不", "bù"))

    def test_everyday_reading_and_particles(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "c.txt")
            with open(path, "w", encoding="utf-8") as f:
                f.write("東西 东西 [dong1 xi1] /east and west/\n東西 东西 [dong1 xi5] /thing/stuff/person/\n"
                        "著 着 [zhao1] /(chess) move/trick/\n著 着 [zhe5] /aspect particle/\n")
            words = tool.load_cedict(path)
        self.assertEqual(words["东西"], ["dong1", "xi5"])
        self.assertEqual(words["着"], ["zhe5"])


if __name__ == "__main__":
    unittest.main()
