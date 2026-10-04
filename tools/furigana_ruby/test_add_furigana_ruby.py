import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import add_furigana_ruby as tool  # noqa: E402


class Align(unittest.TestCase):
    def test_okurigana_stays_bare(self):
        self.assertEqual(tool.align("食べる", "たべる"), [(0, 1, "た")])

    def test_kana_inside_the_word_anchor_the_reading(self):
        self.assertEqual(tool.align("取り引き", "とりひき"), [(0, 1, "と"), (2, 3, "ひ")])

    def test_a_compound_takes_one_reading(self):
        self.assertEqual(tool.align("今日", "きょう"), [(0, 2, "きょう")])

    def test_katakana_reading_is_accepted_as_hiragana(self):
        self.assertEqual(tool.align("東京", "トウキョウ"), [(0, 2, "とうきょう")])

    def test_a_reading_that_does_not_fit_is_refused(self):
        self.assertIsNone(tool.align("食べる", "のむ"))
        self.assertIsNone(tool.align("食べる", "taberu"))
        self.assertIsNone(tool.align("ひらがな", "ひらがな"))
        self.assertIsNone(tool.align("木", "あいうえおかきくけこ"))


class Annotate(unittest.TestCase):
    def annotate(self, doc, answer):
        passages = [p for p, is_text in tool.text_pieces(doc) if is_text]
        return tool.annotate_xhtml(doc, tool.contextual_furigana(passages, lambda batch: answer))

    def test_words_are_annotated_in_order(self):
        out = self.annotate("<p>今日は本を読む。</p>", [[["今日", "きょう"], ["本", "ほん"], ["読む", "よむ"]]])
        self.assertEqual(out, "<p><ruby>今日<rt>きょう</rt></ruby>は<ruby>本<rt>ほん</rt></ruby>を"
                              "<ruby>読<rt>よ</rt></ruby>む。</p>")

    def test_a_word_not_in_the_sentence_is_skipped_and_the_rest_kept(self):
        out = self.annotate("<p>本を読む。</p>", [[["猫", "ねこ"], ["本", "ほん"], ["読む", "たべる"]]])
        self.assertEqual(out, "<p><ruby>本<rt>ほん</rt></ruby>を読む。</p>")

    def test_the_books_own_ruby_is_kept(self):
        doc = "<p><ruby>林<rt>はやし</rt></ruby>さんの本</p>"
        out = self.annotate(doc, [[["本", "ほん"]]])
        self.assertIn("<ruby>林<rt>はやし</rt></ruby>", out)
        self.assertIn("<ruby>本<rt>ほん</rt></ruby>", out)

    def test_no_answer_leaves_the_document_alone(self):
        doc = "<p>本を読む。</p>"
        self.assertEqual(self.annotate(doc, None), doc)


if __name__ == "__main__":
    unittest.main()
