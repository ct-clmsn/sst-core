# -*- coding: utf-8 -*-

from sst_unittest import *
from sst_unittest_support import *

################################################################################
# Code to support a single instance module initialize, must be called setUp method

module_init = 0
module_sema = threading.Semaphore()

def initializeTestModule_SingleInstance(class_inst):
    global module_init
    global module_sema

    module_sema.acquire()
    if module_init != 1:
        # Put your single instance Init Code Here
        module_init = 1
    module_sema.release()

################################################################################

class testcase_ParamComponent(SSTTestCase):

    def initializeClass(self, testName):
        super(type(self), self).initializeClass(testName)
        # Put test based setup code here. it is called before testing starts
        # NOTE: This method is called once for every test

    def setUp(self):
        super(type(self), self).setUp()
        initializeTestModule_SingleInstance(self)
        # Put test based setup code here. it is called once before every test

    def tearDown(self):
        # Put test based teardown code here. it is called once after every test
        super(type(self), self).tearDown()

#####

    def test_ParamComponent(self):
        self.param_component_test_template("param_component")

#####

    def param_component_test_template(self, testtype):
        testsuitedir = self.get_testsuite_dir()
        outdir = test_output_get_run_dir()

        sdlfile = "{0}/test_ParamComponent.py".format(testsuitedir)
        reffile = "{0}/refFiles/test_ParamComponent.out".format(testsuitedir)
        outfile = "{0}/test_ParamComponent.out".format(outdir)

        self.run_sst(sdlfile, outfile)

        # Perform the test
        filter1 = StartsWithFilter("WARNING: No components are")
        filter2 = StartsWithFilter("#")
        ws_filter = IgnoreWhiteSpaceFilter()
        cmp_result = testing_compare_filtered_diff(testtype, outfile, reffile, True, [filter1, filter2, ws_filter])
        self.assertTrue(cmp_result, "Output/Compare file {0} does not match Reference File {1}".format(outfile, reffile))
