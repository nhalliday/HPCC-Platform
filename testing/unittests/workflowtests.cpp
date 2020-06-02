#ifdef _USE_CPPUNIT
#include "unittests.hpp"

class WorkflowPracticeTest : public CppUnit::TestFixture
{
public:
    CPPUNIT_TEST_SUITE(WorkflowPracticeTest);
        CPPUNIT_TEST(testPractice);
    CPPUNIT_TEST_SUITE_END();
protected:
    void testPractice()
    {
        ASSERT(false);
    }

};

CPPUNIT_TEST_SUITE_REGISTRATION(WorkflowPracticeTest);
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(WorkflowPracticeTest, "WorkflowPracticeTest");


#endif