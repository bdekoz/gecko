
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# some parts of this originally taken from /testing/talos/talos/output.py

"""output raptor test results"""
from __future__ import absolute_import

import filters

import json
import os

from logger.logger import RaptorLogger

LOG = RaptorLogger(component='raptor-output')


class Output(object):
    """class for raptor output"""

    def __init__(self, results, supporting_data, subtest_alert_on):
        """
        - results : list of RaptorTestResult instances
        """
        self.results = results
        self.summarized_results = {}
        self.supporting_data = supporting_data
        self.summarized_supporting_data = []
        self.summarized_screenshots = []
        self.subtest_alert_on = subtest_alert_on

    def summarize(self, test_names):
        suites = []
        test_results = {
            'framework': {
                'name': 'raptor',
            },
            'suites': suites,
        }

        # check if we actually have any results
        if len(self.results) == 0:
            LOG.error("no raptor test results found for %s" %
                      ', '.join(test_names))
            return

        for test in self.results:
            vals = []
            subtests = []
            suite = {
                'name': test.name,
                'type': test.type,
                'extraOptions': test.extra_options,
                'subtests': subtests,
                'lowerIsBetter': test.lower_is_better,
                'unit': test.unit,
                'alertThreshold': float(test.alert_threshold)
            }

            # Check if optional properties have been set by the test
            if hasattr(test, "alert_change_type"):
                suite['alertChangeType'] = test.alert_change_type

            # if cold load add that info to the suite result dict; this will be used later
            # when combining the results from multiple browser cycles into one overall result
            if test.cold is True:
                suite['cold'] = True
                suite['browser_cycle'] = int(test.browser_cycle)
                suite['expected_browser_cycles'] = int(test.expected_browser_cycles)

            suites.append(suite)

            # process results for pageloader type of tests
            if test.type in ("pageload", "scenario"):
                # each test can report multiple measurements per pageload
                # each measurement becomes a subtest inside the 'suite'

                # this is the format we receive the results in from the pageload test
                # i.e. one test (subtest) in raptor-firefox-tp6:

                # {u'name': u'raptor-firefox-tp6-amazon', u'type': u'pageload', u'measurements':
                # {u'fnbpaint': [788, 315, 334, 286, 318, 276, 296, 296, 292, 285, 268, 277, 274,
                # 328, 295, 290, 286, 270, 279, 280, 346, 303, 308, 398, 281]}, u'browser':
                # u'Firefox 62.0a1 20180528123052', u'lower_is_better': True, u'page':
                # u'https://www.amazon.com/s/url=search-alias%3Daps&field-keywords=laptop',
                # u'unit': u'ms', u'alert_threshold': 2}

                for measurement_name, replicates in test.measurements.iteritems():
                    new_subtest = {}
                    new_subtest['name'] = measurement_name
                    new_subtest['replicates'] = replicates
                    new_subtest['lowerIsBetter'] = test.subtest_lower_is_better
                    new_subtest['alertThreshold'] = float(test.alert_threshold)
                    new_subtest['value'] = 0
                    new_subtest['unit'] = test.subtest_unit

                    if test.cold is False:
                        # for warm page-load, ignore first value due to 1st pageload noise
                        LOG.info("ignoring the first %s value due to initial pageload noise"
                                 % measurement_name)
                        filtered_values = filters.ignore_first(new_subtest['replicates'], 1)
                    else:
                        # for cold-load we want all the values
                        filtered_values = new_subtest['replicates']

                    # for pageload tests that measure TTFI: TTFI is not guaranteed to be available
                    # everytime; the raptor measure.js webext will substitute a '-1' value in the
                    # cases where TTFI is not available, which is acceptable; however we don't want
                    # to include those '-1' TTFI values in our final results calculations
                    if measurement_name == "ttfi":
                        filtered_values = filters.ignore_negative(filtered_values)
                        # we've already removed the first pageload value; if there aren't any more
                        # valid TTFI values available for this pageload just remove it from results
                        if len(filtered_values) < 1:
                            continue

                    # if 'alert_on' is set for this particular measurement, then we want to set the
                    # flag in the perfherder output to turn on alerting for this subtest
                    if self.subtest_alert_on is not None:
                        if measurement_name in self.subtest_alert_on:
                            LOG.info("turning on subtest alerting for measurement type: %s"
                                     % measurement_name)
                            new_subtest['shouldAlert'] = True

                    new_subtest['value'] = filters.median(filtered_values)

                    vals.append([new_subtest['value'], new_subtest['name']])
                    subtests.append(new_subtest)

            elif test.type == "benchmark":
                if 'assorted-dom' in test.measurements:
                    subtests, vals = self.parseAssortedDomOutput(test)
                elif 'motionmark' in test.measurements:
                    subtests, vals = self.parseMotionmarkOutput(test)
                elif 'speedometer' in test.measurements:
                    subtests, vals = self.parseSpeedometerOutput(test)
                elif 'sunspider' in test.measurements:
                    subtests, vals = self.parseSunspiderOutput(test)
                elif 'unity-webgl' in test.measurements:
                    subtests, vals = self.parseUnityWebGLOutput(test)
                elif 'wasm-godot' in test.measurements:
                    subtests, vals = self.parseWASMGodotOutput(test)
                elif 'wasm-misc' in test.measurements:
                    subtests, vals = self.parseWASMMiscOutput(test)
                elif 'webaudio' in test.measurements:
                    subtests, vals = self.parseWebaudioOutput(test)
                elif 'youtube-playbackperf-test' in test.measurements:
                    subtests, vals = self.parseYoutubePlaybackPerformanceOutput(test)
                suite['subtests'] = subtests

            else:
                LOG.error("output.summarize received unsupported test results type for %s" %
                          test.name)
                return

            # for benchmarks there is generally  more than one subtest in each cycle
            # and a benchmark-specific formula is needed to calculate the final score

            # for pageload tests, if there are > 1 subtests here, that means there
            # were multiple measurements captured in each single pageload; we want
            # to get the mean of those values and report 1 overall 'suite' value
            # for the page; so that each test page/URL only has 1 line output
            # on treeherder/perfherder (all replicates available in the JSON)

            # summarize results for both benchmark or pageload type tests
            if len(subtests) > 1:
                suite['value'] = self.construct_summary(vals, testname=test.name)

        self.summarized_results = test_results

    def combine_browser_cycles(self):
        '''
        At this point the results have been summarized; however there may have been multiple
        browser cycles (i.e. cold load). In which case the results have one entry for each
        test for each browser cycle. For each test we need to combine the results for all
        browser cycles into one results entry.

        For example, this is what the summarized results suites list looks like from a test that
        was run with multiple (two) browser cycles:

        [{'expected_browser_cycles': 2, 'extraOptions': [],
            'name': u'raptor-tp6m-amazon-geckoview-cold', 'lowerIsBetter': True,
            'alertThreshold': 2.0, 'value': 1776.94, 'browser_cycle': 1,
            'subtests': [{'name': u'dcf', 'lowerIsBetter': True, 'alertThreshold': 2.0,
                'value': 818, 'replicates': [818], 'unit': u'ms'}, {'name': u'fcp',
                'lowerIsBetter': True, 'alertThreshold': 2.0, 'value': 1131, 'shouldAlert': True,
                'replicates': [1131], 'unit': u'ms'}, {'name': u'fnbpaint', 'lowerIsBetter': True,
                'alertThreshold': 2.0, 'value': 1056, 'replicates': [1056], 'unit': u'ms'},
                {'name': u'ttfi', 'lowerIsBetter': True, 'alertThreshold': 2.0, 'value': 18074,
                'replicates': [18074], 'unit': u'ms'}, {'name': u'loadtime', 'lowerIsBetter': True,
                'alertThreshold': 2.0, 'value': 1002, 'shouldAlert': True, 'replicates': [1002],
                'unit': u'ms'}],
            'cold': True, 'type': u'pageload', 'unit': u'ms'},
        {'expected_browser_cycles': 2, 'extraOptions': [],
            'name': u'raptor-tp6m-amazon-geckoview-cold', 'lowerIsBetter': True,
            'alertThreshold': 2.0, 'value': 840.25, 'browser_cycle': 2,
            'subtests': [{'name': u'dcf', 'lowerIsBetter': True, 'alertThreshold': 2.0,
                'value': 462, 'replicates': [462], 'unit': u'ms'}, {'name': u'fcp',
                'lowerIsBetter': True, 'alertThreshold': 2.0, 'value': 718, 'shouldAlert': True,
                'replicates': [718], 'unit': u'ms'}, {'name': u'fnbpaint', 'lowerIsBetter': True,
                'alertThreshold': 2.0, 'value': 676, 'replicates': [676], 'unit': u'ms'},
                {'name': u'ttfi', 'lowerIsBetter': True, 'alertThreshold': 2.0, 'value': 3084,
                'replicates': [3084], 'unit': u'ms'}, {'name': u'loadtime', 'lowerIsBetter': True,
                'alertThreshold': 2.0, 'value': 605, 'shouldAlert': True, 'replicates': [605],
                'unit': u'ms'}],
            'cold': True, 'type': u'pageload', 'unit': u'ms'}]

        Need to combine those into a single entry.
        '''
        # check if we actually have any results
        if len(self.results) == 0:
            LOG.info("error: no raptor test results found, so no need to combine browser cycles")
            return

        # first build a list of entries that need to be combined; and as we do that, mark the
        # original suite entry as up for deletion, so once combined we know which ones to del
        # note that summarized results are for all tests that were ran in the session, which
        # could include cold and / or warm page-load and / or benchnarks combined
        suites_to_be_combined = []
        combined_suites = []

        for _index, suite in enumerate(self.summarized_results.get('suites', [])):
            if suite.get('cold') is None:
                continue

            if suite['expected_browser_cycles'] > 1:
                _name = suite['name']
                _details = suite.copy()
                suites_to_be_combined.append({'name': _name, 'details': _details})
                suite['to_be_deleted'] = True

        # now create a new suite entry that will have all the results from
        # all of the browser cycles, but in one result entry for each test
        combined_suites = {}

        for next_suite in suites_to_be_combined:
            suite_name = next_suite['details']['name']
            browser_cycle = next_suite['details']['browser_cycle']
            LOG.info("combining results from browser cycle %d for %s"
                     % (browser_cycle, suite_name))
            if browser_cycle == 1:
                # first browser cycle so just take entire entry to start with
                combined_suites[suite_name] = next_suite['details']
                LOG.info("created new combined result with intial cycle replicates")
                # remove the 'cold', 'browser_cycle', and 'expected_browser_cycles' info
                # as we don't want that showing up in perfherder data output
                del(combined_suites[suite_name]['cold'])
                del(combined_suites[suite_name]['browser_cycle'])
                del(combined_suites[suite_name]['expected_browser_cycles'])
            else:
                # subsequent browser cycles, already have an entry; just add subtest replicates
                for next_subtest in next_suite['details']['subtests']:
                    # find the existing entry for that subtest in our new combined test entry
                    found_subtest = False
                    for combined_subtest in combined_suites[suite_name]['subtests']:
                        if combined_subtest['name'] == next_subtest['name']:
                            # add subtest (measurement type) replicates to the combined entry
                            LOG.info("adding replicates for %s" % next_subtest['name'])
                            combined_subtest['replicates'].extend(next_subtest['replicates'])
                            found_subtest = True
                    # the subtest / measurement type wasn't found in our existing combined
                    # result entry; if it is for the same suite name add it - this could happen
                    # as ttfi may not be available in every browser cycle
                    if not found_subtest:
                        LOG.info("adding replicates for %s" % next_subtest['name'])
                        combined_suites[next_suite['details']['name']]['subtests'] \
                            .append(next_subtest)

        # now we have a single entry for each test; with all replicates from all browser cycles
        for i, name in enumerate(combined_suites):
            vals = []
            for next_sub in combined_suites[name]['subtests']:
                # calculate sub-test results (i.e. each measurement type)
                next_sub['value'] = filters.median(next_sub['replicates'])
                # add to vals; vals is used to calculate overall suite result i.e. the
                # geomean of all of the subtests / measurement types
                vals.append([next_sub['value'], next_sub['name']])

            # calculate overall suite result ('value') which is geomean of all measures
            if len(combined_suites[name]['subtests']) > 1:
                combined_suites[name]['value'] = self.construct_summary(vals, testname=name)

            # now add the combined suite entry to our overall summarized results!
            self.summarized_results['suites'].append(combined_suites[name])

        # now it is safe to delete the original entries that were made by each cycle
        self.summarized_results['suites'] = [item for item in self.summarized_results['suites']
                                             if item.get('to_be_deleted') is not True]

    def summarize_supporting_data(self):
        '''
        Supporting data was gathered outside of the main raptor test; it will be kept
        separate from the main raptor test results. Summarize it appropriately.

        supporting_data = {'type': 'data-type',
                           'test': 'raptor-test-ran-when-data-was-gathered',
                           'unit': 'unit that the values are in',
                           'values': {
                               'name': value,
                               'nameN': valueN}}

        More specifically, power data will look like this:

        supporting_data = {'type': 'power',
                           'test': 'raptor-speedometer-geckoview',
                           'unit': 'mAh',
                           'values': {
                               'cpu': cpu,
                               'wifi': wifi,
                               'screen': screen,
                               'proportional': proportional}}

        We want to treat each value as a 'subtest'; and for the overall aggregated
        test result we will add all of these subtest values togther.
        '''
        if self.supporting_data is None:
            return

        self.summarized_supporting_data = []

        for data_set in self.supporting_data:

            suites = []
            test_results = {
                'framework': {
                    'name': 'raptor',
                },
                'suites': suites,
            }

            data_type = data_set['type']
            LOG.info("summarizing %s data" % data_type)

            # suite name will be name of the actual raptor test that ran, plus the type of
            # supporting data i.e. 'raptor-speedometer-geckoview-power'
            vals = []
            subtests = []
            suite = {
                'name': data_set['test'] + "-" + data_set['type'],
                'type': data_set['type'],
                'subtests': subtests,
                'lowerIsBetter': True,
                'unit': data_set['unit'],
                'alertThreshold': 2.0
            }

            suites.append(suite)

            # each supporting data measurement becomes a subtest, with the measurement type
            # used for the subtest name. i.e. 'raptor-speedometer-geckoview-power-cpu'
            # the overall 'suite' value for supporting data will be the sum of all measurements
            for measurement_name, value in data_set['values'].iteritems():
                new_subtest = {}
                new_subtest['name'] = data_set['test'] + "-" + data_type + "-" + measurement_name
                new_subtest['value'] = value
                new_subtest['lowerIsBetter'] = True
                new_subtest['alertThreshold'] = 2.0
                new_subtest['unit'] = data_set['unit']
                subtests.append(new_subtest)
                vals.append([new_subtest['value'], new_subtest['name']])

            if len(subtests) > 1:
                suite['value'] = self.construct_summary(vals, testname="supporting_data")

            self.summarized_supporting_data.append(test_results)

        return

    def parseSpeedometerOutput(self, test):
        # each benchmark 'index' becomes a subtest; each pagecycle / iteration
        # of the test has multiple values per index/subtest

        # this is the format we receive the results in from the benchmark
        # i.e. this is ONE pagecycle of speedometer:

        # {u'name': u'raptor-speedometer', u'type': u'benchmark', u'measurements':
        # {u'speedometer': [[{u'AngularJS-TodoMVC/DeletingAllItems': [147.3000000000011,
        # 149.95999999999913, 143.29999999999927, 150.34000000000378, 257.6999999999971],
        # u'Inferno-TodoMVC/CompletingAllItems/Sync': [88.03999999999996,#
        # 85.60000000000036, 94.18000000000029, 95.19999999999709, 86.47999999999593],
        # u'AngularJS-TodoMVC': [518.2400000000016, 525.8199999999997, 610.5199999999968,
        # 532.8200000000215, 640.1800000000003], ...(repeated for each index/subtest)}]]},
        # u'browser': u'Firefox 62.0a1 20180528123052', u'lower_is_better': False, u'page':
        # u'http://localhost:55019/Speedometer/index.html?raptor', u'unit': u'score',
        # u'alert_threshold': 2}

        _subtests = {}
        data = test.measurements['speedometer']
        for page_cycle in data:
            for sub, replicates in page_cycle[0].iteritems():
                # for each pagecycle, build a list of subtests and append all related replicates
                if sub not in _subtests.keys():
                    # subtest not added yet, first pagecycle, so add new one
                    _subtests[sub] = {'unit': test.subtest_unit,
                                      'alertThreshold': float(test.alert_threshold),
                                      'lowerIsBetter': test.subtest_lower_is_better,
                                      'name': sub,
                                      'replicates': []}
                _subtests[sub]['replicates'].extend([round(x, 3) for x in replicates])

        vals = []
        subtests = []
        names = _subtests.keys()
        names.sort(reverse=True)
        for name in names:
            _subtests[name]['value'] = filters.median(_subtests[name]['replicates'])
            subtests.append(_subtests[name])
            vals.append([_subtests[name]['value'], name])

        return subtests, vals

    def parseWASMMiscOutput(self, test):
        '''
          {u'wasm-misc': [
            [[{u'name': u'validate', u'time': 163.44000000000005},
              ...
              {u'name': u'__total__', u'time': 63308.434904788155}]],
            ...
            [[{u'name': u'validate', u'time': 129.42000000000002},
              {u'name': u'__total__', u'time': 63181.24089257814}]]
           ]}
        '''
        _subtests = {}
        data = test.measurements['wasm-misc']
        for page_cycle in data:
            for item in page_cycle[0]:
                # for each pagecycle, build a list of subtests and append all related replicates
                sub = item['name']
                if sub not in _subtests.keys():
                    # subtest not added yet, first pagecycle, so add new one
                    _subtests[sub] = {'unit': test.subtest_unit,
                                      'alertThreshold': float(test.alert_threshold),
                                      'lowerIsBetter': test.subtest_lower_is_better,
                                      'name': sub,
                                      'replicates': []}
                _subtests[sub]['replicates'].append(item['time'])

        vals = []
        subtests = []
        names = _subtests.keys()
        names.sort(reverse=True)
        for name in names:
            _subtests[name]['value'] = filters.median(_subtests[name]['replicates'])
            subtests.append(_subtests[name])
            vals.append([_subtests[name]['value'], name])

        return subtests, vals

    def parseWASMGodotOutput(self, test):
        '''
            {u'wasm-godot': [
                {
                  "name": "wasm-instantiate",
                  "time": 349
                },{
                  "name": "engine-instantiate",
                  "time": 1263
                ...
                }]}
        '''
        _subtests = {}
        data = test.measurements['wasm-godot']
        print (data)
        for page_cycle in data:
            for item in page_cycle[0]:
                # for each pagecycle, build a list of subtests and append all related replicates
                sub = item['name']
                if sub not in _subtests.keys():
                    # subtest not added yet, first pagecycle, so add new one
                    _subtests[sub] = {'unit': test.subtest_unit,
                                      'alertThreshold': float(test.alert_threshold),
                                      'lowerIsBetter': test.subtest_lower_is_better,
                                      'name': sub,
                                      'replicates': []}
                _subtests[sub]['replicates'].append(item['time'])

        vals = []
        subtests = []
        names = _subtests.keys()
        names.sort(reverse=True)
        for name in names:
            _subtests[name]['value'] = filters.median(_subtests[name]['replicates'])
            subtests.append(_subtests[name])
            vals.append([_subtests[name]['value'], name])

        return subtests, vals

    def parseWebaudioOutput(self, test):
        # each benchmark 'index' becomes a subtest; each pagecycle / iteration
        # of the test has multiple values per index/subtest

        # this is the format we receive the results in from the benchmark
        # i.e. this is ONE pagecycle of speedometer:

        # {u'name': u'raptor-webaudio-firefox', u'type': u'benchmark', u'measurements':
        # {u'webaudio': [[u'[{"name":"Empty testcase","duration":26,"buffer":{}},{"name"
        # :"Simple gain test without resampling","duration":66,"buffer":{}},{"name":"Simple
        # gain test without resampling (Stereo)","duration":71,"buffer":{}},{"name":"Simple
        # gain test without resampling (Stereo and positional)","duration":67,"buffer":{}},
        # {"name":"Simple gain test","duration":41,"buffer":{}},{"name":"Simple gain test
        # (Stereo)","duration":59,"buffer":{}},{"name":"Simple gain test (Stereo and positional)",
        # "duration":68,"buffer":{}},{"name":"Upmix without resampling (Mono -> Stereo)",
        # "duration":53,"buffer":{}},{"name":"Downmix without resampling (Mono -> Stereo)",
        # "duration":44,"buffer":{}},{"name":"Simple mixing (same buffer)",
        # "duration":288,"buffer":{}}

        _subtests = {}
        data = test.measurements['webaudio']
        for page_cycle in data:
            data = json.loads(page_cycle[0])
            for item in data:
                # for each pagecycle, build a list of subtests and append all related replicates
                sub = item['name']
                replicates = [item['duration']]
                if sub not in _subtests.keys():
                    # subtest not added yet, first pagecycle, so add new one
                    _subtests[sub] = {'unit': test.subtest_unit,
                                      'alertThreshold': float(test.alert_threshold),
                                      'lowerIsBetter': test.subtest_lower_is_better,
                                      'name': sub,
                                      'replicates': []}
                _subtests[sub]['replicates'].extend([round(x, 3) for x in replicates])

        vals = []
        subtests = []
        names = _subtests.keys()
        names.sort(reverse=True)
        for name in names:
            _subtests[name]['value'] = filters.median(_subtests[name]['replicates'])
            subtests.append(_subtests[name])
            vals.append([_subtests[name]['value'], name])

        print subtests
        return subtests, vals

    def parseMotionmarkOutput(self, test):
        # for motionmark we want the frameLength:average value for each test

        # this is the format we receive the results in from the benchmark
        # i.e. this is ONE pagecycle of motionmark htmlsuite test:composited Transforms:

        # {u'name': u'raptor-motionmark-firefox',
        #  u'type': u'benchmark',
        #  u'measurements': {
        #    u'motionmark':
        #      [[{u'HTMLsuite':
        #        {u'Composited Transforms':
        #          {u'scoreLowerBound': 272.9947975553528,
        #           u'frameLength': {u'average': 25.2, u'stdev': 27.0,
        #                            u'percent': 68.2, u'concern': 39.5},
        #           u'controller': {u'average': 300, u'stdev': 0, u'percent': 0, u'concern': 3},
        #           u'scoreUpperBound': 327.0052024446473,
        #           u'complexity': {u'segment1': [[300, 16.6], [300, 16.6]], u'complexity': 300,
        #                           u'segment2': [[300, None], [300, None]], u'stdev': 6.8},
        #           u'score': 300.00000000000006,
        #           u'complexityAverage': {u'segment1': [[30, 30], [30, 30]], u'complexity': 30,
        #                                  u'segment2': [[300, 300], [300, 300]], u'stdev': None}
        #  }}}]]}}

        _subtests = {}
        data = test.measurements['motionmark']
        for page_cycle in data:
            page_cycle_results = page_cycle[0]

            # TODO: this assumes a single suite is run
            suite = page_cycle_results.keys()[0]
            for sub in page_cycle_results[suite].keys():
                replicate = round(page_cycle_results[suite][sub]['frameLength']['average'], 3)

                if sub not in _subtests.keys():
                    # subtest not added yet, first pagecycle, so add new one
                    _subtests[sub] = {'unit': test.subtest_unit,
                                      'alertThreshold': float(test.alert_threshold),
                                      'lowerIsBetter': test.subtest_lower_is_better,
                                      'name': sub,
                                      'replicates': []}
                _subtests[sub]['replicates'].extend([replicate])

        vals = []
        subtests = []
        names = _subtests.keys()
        names.sort(reverse=True)
        for name in names:
            _subtests[name]['value'] = filters.median(_subtests[name]['replicates'])
            subtests.append(_subtests[name])
            vals.append([_subtests[name]['value'], name])

        return subtests, vals

    def parseSunspiderOutput(self, test):
        _subtests = {}
        data = test.measurements['sunspider']
        for page_cycle in data:
            for sub, replicates in page_cycle[0].iteritems():
                # for each pagecycle, build a list of subtests and append all related replicates
                if sub not in _subtests.keys():
                    # subtest not added yet, first pagecycle, so add new one
                    _subtests[sub] = {'unit': test.subtest_unit,
                                      'alertThreshold': float(test.alert_threshold),
                                      'lowerIsBetter': test.subtest_lower_is_better,
                                      'name': sub,
                                      'replicates': []}
                _subtests[sub]['replicates'].extend([round(x, 3) for x in replicates])

        subtests = []
        vals = []

        names = _subtests.keys()
        names.sort(reverse=True)
        for name in names:
            _subtests[name]['value'] = filters.mean(_subtests[name]['replicates'])
            subtests.append(_subtests[name])

            vals.append([_subtests[name]['value'], name])

        return subtests, vals

    def parseUnityWebGLOutput(self, test):
        """
        Example output (this is one page cycle):

        {'name': 'raptor-unity-webgl-firefox',
         'type': 'benchmark',
         'measurements': {
            'unity-webgl': [
                [
                    '[{"benchmark":"Mandelbrot GPU","result":1035361},...}]'
                ]
            ]
         },
         'lower_is_better': False,
         'unit': 'score'
        }
        """
        _subtests = {}
        data = test.measurements['unity-webgl']
        for page_cycle in data:
            data = json.loads(page_cycle[0])
            for item in data:
                # for each pagecycle, build a list of subtests and append all related replicates
                sub = item['benchmark']
                if sub not in _subtests.keys():
                    # subtest not added yet, first pagecycle, so add new one
                    _subtests[sub] = {'unit': test.subtest_unit,
                                      'alertThreshold': float(test.alert_threshold),
                                      'lowerIsBetter': test.subtest_lower_is_better,
                                      'name': sub,
                                      'replicates': []}
                _subtests[sub]['replicates'].append(item['result'])

        vals = []
        subtests = []
        names = _subtests.keys()
        names.sort(reverse=True)
        for name in names:
            _subtests[name]['value'] = filters.median(_subtests[name]['replicates'])
            subtests.append(_subtests[name])
            vals.append([_subtests[name]['value'], name])

        return subtests, vals

    def parseAssortedDomOutput(self, test):
        # each benchmark 'index' becomes a subtest; each pagecycle / iteration
        # of the test has multiple values

        # this is the format we receive the results in from the benchmark
        # i.e. this is ONE pagecycle of assorted-dom ('test' is a valid subtest name btw):

        # {u'worker-getname-performance-getter': 5.9, u'window-getname-performance-getter': 6.1,
        # u'window-getprop-performance-getter': 6.1, u'worker-getprop-performance-getter': 6.1,
        # u'test': 5.8, u'total': 30}

        # the 'total' is provided for us from the benchmark; the overall score will be the mean of
        # the totals from all pagecycles; but keep all the subtest values for the logs/json

        _subtests = {}
        data = test.measurements['assorted-dom']
        for pagecycle in data:
            for _sub, _value in pagecycle[0].iteritems():
                # build a list of subtests and append all related replicates
                if _sub not in _subtests.keys():
                    # subtest not added yet, first pagecycle, so add new one
                    _subtests[_sub] = {'unit': test.subtest_unit,
                                       'alertThreshold': float(test.alert_threshold),
                                       'lowerIsBetter': test.subtest_lower_is_better,
                                       'name': _sub,
                                       'replicates': []}
                _subtests[_sub]['replicates'].extend([_value])

        vals = []
        subtests = []
        names = _subtests.keys()
        names.sort(reverse=True)
        for name in names:
            _subtests[name]['value'] = round(filters.median(_subtests[name]['replicates']), 2)
            subtests.append(_subtests[name])
            # only use the 'total's to compute the overall result
            if name == 'total':
                vals.append([_subtests[name]['value'], name])

        return subtests, vals

    def parseYoutubePlaybackPerformanceOutput(self, test):
        """Parse the metrics for the Youtube playback performance test.

        For each video measured values for dropped and decoded frames will be
        available from the benchmark site.

        {u'PlaybackPerf.VP9.2160p60@2X': {u'droppedFrames': 1, u'decodedFrames': 796}

        With each page cycle / iteration of the test multiple values can be present.

        Raptor will calculate the percentage of dropped frames to decoded frames.
        All those three values will then be emitted as separate sub tests.
        """
        _subtests = {}
        data = test.measurements['youtube-playbackperf-test']

        def create_subtest_entry(name, value,
                                 unit=test.subtest_unit,
                                 lower_is_better=test.subtest_lower_is_better):
            # build a list of subtests and append all related replicates
            if name not in _subtests.keys():
                # subtest not added yet, first pagecycle, so add new one
                _subtests[name] = {
                    'name': name,
                    'unit': unit,
                    'lowerIsBetter': lower_is_better,
                    'replicates': [],
                }

            _subtests[name]['replicates'].append(value)

        for pagecycle in data:
            for _sub, _value in pagecycle[0].iteritems():
                try:
                    percent_dropped = float(_value['droppedFrames']) / _value['decodedFrames'] \
                                      * 100.0
                except ZeroDivisionError:
                    # if no frames have been decoded the playback failed completely
                    percent_dropped = 100.0

                # Remove the not needed "PlaybackPerf." prefix from each test
                _sub = _sub.split('PlaybackPerf.', 1)[-1]

                # build a list of subtests and append all related replicates
                create_subtest_entry("{}_decoded_frames".format(_sub),
                                     _value['decodedFrames'],
                                     lower_is_better=False,
                                     )
                create_subtest_entry("{}_dropped_frames".format(_sub),
                                     _value['droppedFrames'],
                                     )
                create_subtest_entry("{}_%_dropped_frames".format(_sub),
                                     percent_dropped,
                                     )

        vals = []
        subtests = []
        names = _subtests.keys()
        names.sort(reverse=True)
        for name in names:
            _subtests[name]['value'] = round(filters.median(_subtests[name]['replicates']), 2)
            subtests.append(_subtests[name])
            if name.endswith("dropped_frames"):
                vals.append([_subtests[name]['value'], name])

        return subtests, vals

    def summarize_screenshots(self, screenshots):
        if len(screenshots) == 0:
            return

        self.summarized_screenshots.append("""<!DOCTYPE html>
        <head>
        <style>
            table, th, td {
              border: 1px solid black;
              border-collapse: collapse;
            }
        </style>
        </head>
        <html> <body>
        <h1>Captured screenshots!</h1>
        <table style="width:100%">
          <tr>
            <th>Test Name</th>
            <th>Pagecycle</th>
            <th>Screenshot</th>
          </tr>""")

        for screenshot in screenshots:
            self.summarized_screenshots.append("""<tr>
            <th>%s</th>
            <th> %s</th>
            <th>
                <img src="%s" alt="%s %s" width="320" height="240">
            </th>
            </tr>""" % (screenshot['test_name'],
                        screenshot['page_cycle'],
                        screenshot['screenshot'],
                        screenshot['test_name'],
                        screenshot['page_cycle']))

        self.summarized_screenshots.append("""</table></body> </html>""")

    def output(self, test_names):
        """output to file and perfherder data json """
        if os.getenv('MOZ_UPLOAD_DIR'):
            # i.e. testing/mozharness/build/raptor.json locally; in production it will
            # be at /tasks/task_*/build/ (where it will be picked up by mozharness later
            # and made into a tc artifact accessible in treeherder as perfherder-data.json)
            results_path = os.path.join(os.path.dirname(os.environ['MOZ_UPLOAD_DIR']),
                                        'raptor.json')
            screenshot_path = os.path.join(os.path.dirname(os.environ['MOZ_UPLOAD_DIR']),
                                           'screenshots.html')
        else:
            results_path = os.path.join(os.getcwd(), 'raptor.json')
            screenshot_path = os.path.join(os.getcwd(), 'screenshots.html')

        if self.summarized_results == {}:
            LOG.error("no summarized raptor results found for %s" %
                      ', '.join(test_names))
        else:
            with open(results_path, 'w') as f:
                for result in self.summarized_results:
                    f.write("%s\n" % result)

        if len(self.summarized_screenshots) > 0:
            with open(screenshot_path, 'w') as f:
                for result in self.summarized_screenshots:
                    f.write("%s\n" % result)
            LOG.info("screen captures can be found locally at: %s" % screenshot_path)

        # now that we've checked for screen captures too, if there were no actual
        # test results we can bail out here
        if self.summarized_results == {}:
            return False

        # when gecko_profiling, we don't want results ingested by Perfherder
        extra_opts = self.summarized_results['suites'][0].get('extraOptions', [])
        if 'gecko_profile' not in extra_opts:
            # if we have supporting data i.e. power, we ONLY want those measurements
            # dumped out. TODO: Bug 1515406 - Add option to output both supplementary
            # data (i.e. power) and the regular Raptor test result
            # Both are already available as separate PERFHERDER_DATA json blobs
            if len(self.summarized_supporting_data) == 0:
                LOG.info("PERFHERDER_DATA: %s" % json.dumps(self.summarized_results))
            else:
                LOG.info("supporting data measurements exist - only posting those to perfherder")
        else:
            LOG.info("gecko profiling enabled - not posting results for perfherder")

        json.dump(self.summarized_results, open(results_path, 'w'), indent=2,
                  sort_keys=True)
        LOG.info("results can also be found locally at: %s" % results_path)

        return True

    def output_supporting_data(self, test_names):
        '''
        Supporting data was gathered outside of the main raptor test; it has already
        been summarized, now output it appropriately.

        We want to output supporting data in a completely separate perfherder json blob and
        in a corresponding file artifact. This way supporting data can be ingested as it's own
        test suite in perfherder and alerted upon if desired. Kept outside of the test results
        from the actual Raptor test that was ran when the supporting data was gathered.
        '''
        if len(self.summarized_supporting_data) == 0:
            LOG.error("no summarized supporting data found for %s" %
                      ', '.join(test_names))
            return False

        for next_data_set in self.summarized_supporting_data:
            data_type = next_data_set['suites'][0]['type']

            if os.environ['MOZ_UPLOAD_DIR']:
                # i.e. testing/mozharness/build/raptor.json locally; in production it will
                # be at /tasks/task_*/build/ (where it will be picked up by mozharness later
                # and made into a tc artifact accessible in treeherder as perfherder-data.json)
                results_path = os.path.join(os.path.dirname(os.environ['MOZ_UPLOAD_DIR']),
                                            'raptor-%s.json' % data_type)
            else:
                results_path = os.path.join(os.getcwd(), 'raptor-%s.json' % data_type)

            # dump data to raptor-data.json artifact
            json.dump(next_data_set, open(results_path, 'w'), indent=2, sort_keys=True)

            # the output that treeherder expects to find
            LOG.info("PERFHERDER_DATA: %s" % json.dumps(next_data_set))
            LOG.info("%s results can also be found locally at: %s" % (data_type, results_path))

        return True

    @classmethod
    def v8_Metric(cls, val_list):
        results = [i for i, j in val_list]
        score = 100 * filters.geometric_mean(results)
        return score

    @classmethod
    def JS_Metric(cls, val_list):
        """v8 benchmark score"""
        results = [i for i, j in val_list]
        return sum(results)

    @classmethod
    def speedometer_score(cls, val_list):
        """
        speedometer_score: https://bug-172968-attachments.webkit.org/attachment.cgi?id=319888
        """
        correctionFactor = 3
        results = [i for i, j in val_list]
        # speedometer has 16 tests, each of these are made of up 9 subtests
        # and a sum of the 9 values.  We receive 160 values, and want to use
        # the 16 test values, not the sub test values.
        if len(results) != 160:
            raise Exception("Speedometer has 160 subtests, found: %s instead" % len(results))

        results = results[9::10]
        score = 60 * 1000 / filters.geometric_mean(results) / correctionFactor
        return score

    @classmethod
    def benchmark_score(cls, val_list):
        """
        benchmark_score: ares6/jetstream self reported as 'geomean'
        """
        results = [i for i, j in val_list if j == 'geomean']
        return filters.mean(results)

    @classmethod
    def webaudio_score(cls, val_list):
        """
        webaudio_score: self reported as 'Geometric Mean'
        """
        results = [i for i, j in val_list if j == 'Geometric Mean']
        return filters.mean(results)

    @classmethod
    def unity_webgl_score(cls, val_list):
        """
        unity_webgl_score: self reported as 'Geometric Mean'
        """
        results = [i for i, j in val_list if j == 'Geometric Mean']
        return filters.mean(results)

    @classmethod
    def wasm_misc_score(cls, val_list):
        """
        wasm_misc_score: self reported as '__total__'
        """
        results = [i for i, j in val_list if j == '__total__']
        return filters.mean(results)

    @classmethod
    def wasm_godot_score(cls, val_list):
        """
        wasm_godot_score: first-interactive mean
        """
        results = [i for i, j in val_list if j == 'first-interactive']
        return filters.mean(results)

    @classmethod
    def stylebench_score(cls, val_list):
        """
        stylebench_score: https://bug-172968-attachments.webkit.org/attachment.cgi?id=319888
        """
        correctionFactor = 3
        results = [i for i, j in val_list]

        # stylebench has 5 tests, each of these are made of up 5 subtests
        #
        #   * Adding classes.
        #   * Removing classes.
        #   * Mutating attributes.
        #   * Adding leaf elements.
        #   * Removing leaf elements.
        #
        # which are made of two subtests each (sync/async) and repeated 5 times
        # each, thus, the list here looks like:
        #
        #   [Test name/Adding classes - 0/ Sync; <x>]
        #   [Test name/Adding classes - 0/ Async; <y>]
        #   [Test name/Adding classes - 0; <x> + <y>]
        #   [Test name/Removing classes - 0/ Sync; <x>]
        #   [Test name/Removing classes - 0/ Async; <y>]
        #   [Test name/Removing classes - 0; <x> + <y>]
        #   ...
        #   [Test name/Adding classes - 1 / Sync; <x>]
        #   [Test name/Adding classes - 1 / Async; <y>]
        #   [Test name/Adding classes - 1 ; <x> + <y>]
        #   ...
        #   [Test name/Removing leaf elements - 4; <x> + <y>]
        #   [Test name; <sum>] <- This is what we want.
        #
        # So, 5 (subtests) *
        #     5 (repetitions) *
        #     3 (entries per repetition (sync/async/sum)) =
        #     75 entries for test before the sum.
        #
        # We receive 76 entries per test, which ads up to 380. We want to use
        # the 5 test entries, not the rest.
        if len(results) != 380:
            raise Exception("StyleBench has 380 entries, found: %s instead" % len(results))

        results = results[75::76]
        score = 60 * 1000 / filters.geometric_mean(results) / correctionFactor
        return score

    @classmethod
    def sunspider_score(cls, val_list):
        results = [i for i, j in val_list]
        return sum(results)

    @classmethod
    def assorted_dom_score(cls, val_list):
        results = [i for i, j in val_list]
        return round(filters.geometric_mean(results), 2)

    @classmethod
    def youtube_playback_performance_score(cls, val_list):
        """Calculate percentage of failed tests."""
        results = [i for i, j in val_list]
        return round(filters.mean(results), 2)

    @classmethod
    def supporting_data_total(cls, val_list):
        results = [i for i, j in val_list]
        return sum(results)

    def construct_summary(self, vals, testname):
        if testname.startswith('raptor-v8_7'):
            return self.v8_Metric(vals)
        elif testname.startswith('raptor-kraken'):
            return self.JS_Metric(vals)
        elif testname.startswith('raptor-jetstream'):
            return self.benchmark_score(vals)
        elif testname.startswith('raptor-speedometer'):
            return self.speedometer_score(vals)
        elif testname.startswith('raptor-stylebench'):
            return self.stylebench_score(vals)
        elif testname.startswith('raptor-sunspider'):
            return self.sunspider_score(vals)
        elif testname.startswith('raptor-unity-webgl'):
            return self.unity_webgl_score(vals)
        elif testname.startswith('raptor-webaudio'):
            return self.webaudio_score(vals)
        elif testname.startswith('raptor-assorted-dom'):
            return self.assorted_dom_score(vals)
        elif testname.startswith('raptor-wasm-misc'):
            return self.wasm_misc_score(vals)
        elif testname.startswith('raptor-wasm-godot'):
            return self.wasm_godot_score(vals)
        elif testname.startswith('raptor-youtube-playback'):
            return self.youtube_playback_performance_score(vals)
        elif testname.startswith('supporting_data'):
            return self.supporting_data_total(vals)
        elif len(vals) > 1:
            return round(filters.geometric_mean([i for i, j in vals]), 2)
        else:
            return round(filters.mean([i for i, j in vals]), 2)
