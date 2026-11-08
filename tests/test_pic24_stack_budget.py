"""Regression checks for the XC16 fixed-size stack adjustment spellings."""
from pathlib import Path
import tempfile
import unittest

from pic24_stack_budget import analyze, parse_functions


class StackBudgetTests(unittest.TestCase):
    def assembly(self, directory, instructions):
        path = Path(directory) / 'probe.s'
        path.write_text('\t.type _probe,@function\n_probe:\n' + instructions +
                        '\n\t.size _probe, .-_probe\n')
        return path

    def test_literal_adjustments_have_identical_bounds(self):
        for addition, subtraction in (('add #34,w15', 'sub #34,w15'),
                                      ('add w15,#34,w15', 'sub w15,#34,w15')):
            with self.subTest(addition=addition), tempfile.TemporaryDirectory() as directory:
                path = self.assembly(directory, addition + '\nmov.d w8,[w15++]\n' + subtraction + '\nreturn')
                self.assertEqual(parse_functions(path)['_probe']['own_bound'], 38)
                self.assertEqual(analyze([path], 42)['conservative_internal_bytes'], 42)
                with self.assertRaises(ValueError):
                    analyze([path], 41)

    def test_variable_stack_growth_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.assembly(directory, 'add w0,w15\nreturn')
            with self.assertRaises(ValueError):
                parse_functions(path)


if __name__ == '__main__':
    unittest.main()
