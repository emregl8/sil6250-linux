import importlib.util
import io
from pathlib import Path
import unittest


SPEC = importlib.util.spec_from_file_location('verification_report',
    Path(__file__).resolve().parents[1] / 'tools/summarize_verification.py')
REPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPORT)
HEADER = 'trial_id,kind,result,first_result,attempts,latency_ms\n'


class VerificationReportTests(unittest.TestCase):
    def report(self, rows):
        return REPORT.summarize(io.StringIO(HEADER + rows), 5)

    def test_retries_and_request_errors_are_separate(self):
        result = self.report('1,genuine,match,no-match,2,500\n'
                             '2,genuine,no-match,no-match,3,1000\n'
                             '3,genuine,timeout,timeout,0,45000\n'
                             '4,impostor,no-match,no-match,3,800\n')
        genuine = result['groups']['genuine']
        self.assertEqual(genuine['trials'], 3)
        self.assertEqual(genuine['error_rate'], 0.5)
        self.assertEqual(genuine['first_placement_error_rate'], 1)
        self.assertEqual(genuine['outcomes']['timeout'], 1)
        self.assertEqual(result['groups']['impostor']['error_rate'], 0)

    def test_zero_false_accepts_does_not_mean_zero_uncertainty(self):
        result = self.report('1,impostor,no-match,no-match,1,100\n')
        interval = result['groups']['impostor']['wilson_95_interval']
        self.assertAlmostEqual(interval[0], 0)
        self.assertGreater(interval[1], 0.7)
        self.assertIsNone(result['groups']['genuine']['error_rate'])

    def test_false_accept_is_counted(self):
        result = self.report('1,impostor,match,no-match,2,200\n')
        self.assertEqual(result['groups']['impostor']['error_rate'], 1)
        self.assertEqual(result['groups']['impostor']['first_placement_error_rate'], 0)

    def test_invalid_or_duplicate_trials_are_rejected(self):
        rows = ['1,genuine,match,match,1,nan\n',
                '1,genuine,match,match,1,-1\n',
                '1,genuine,match,match,0,1\n',
                '1,genuine,match,match,4,1\n',
                '1,genuine,no-match,match,1,1\n',
                '1,genuine,match,match,2,1\n',
                '1,unknown,match,match,1,1\n',
                '1,genuine,unknown,match,1,1\n',
                '1,genuine,match\n',
                '1,genuine,match,match,1,1,extra\n',
                '1,genuine,match,match,1,1\n1,genuine,match,match,1,1\n', '']
        for row in rows:
            with self.subTest(row=row), self.assertRaises(ValueError):
                self.report(row)

    def test_header_and_threshold_are_required(self):
        with self.assertRaises(ValueError):
            REPORT.summarize(io.StringIO('raw_image\nsecret\n'), 5)
        with self.assertRaises(ValueError):
            REPORT.summarize(io.StringIO(HEADER), 0)


if __name__ == '__main__':
    unittest.main()
