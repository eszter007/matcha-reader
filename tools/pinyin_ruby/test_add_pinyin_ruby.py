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


if __name__ == "__main__":
    unittest.main()
