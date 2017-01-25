#ifndef DASH__TEST__TEAM_LOCALITY_TEST_H_
#define DASH__TEST__TEAM_LOCALITY_TEST_H_

#include "TestBase.h"

/**
 * Test fixture for class dash::TeamLocality
 */
class TeamLocalityTest : public dash::test::TestBase {
protected:

  TeamLocalityTest() {
    LOG_MESSAGE(">>> Test suite: TeamLocalityTest");
  }

  virtual ~TeamLocalityTest() {
    LOG_MESSAGE("<<< Closing test suite: TeamLocalityTest");
  }

  virtual void SetUp() {
    dash::init(&TESTENV.argc, &TESTENV.argv);
    dash::Team::All().barrier();
    LOG_MESSAGE("===> Running test case with %d units ...", dash::size());
  }

  virtual void TearDown() {
    dash::Team::All().barrier();
    LOG_MESSAGE("<=== Finished test case with %d units", dash::size());
    dash::finalize();
  }
};

#endif // DASH__TEST__TEAM_LOCALITY_TEST_H_
