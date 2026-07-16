#include "TestFramework.h"

#include "Game/BotNavigation.h"

#include <cstddef>
#include <limits>

TEST(BotNavigation, UsesDeterministicDirectedAuthoredRoute) {
    BotNavigationConfig config;
    config.maxEdgeLength = 10.0f;
    config.maxDirectRouteLength = 2.0f;
    BotNavigation navigation(config);

    ASSERT_TRUE(navigation.AddWaypoint(3, Vector3(5.0f, 5.0f, 0.0f)));
    ASSERT_TRUE(navigation.AddWaypoint(1, Vector3(0.0f, 0.0f, 0.0f)));
    ASSERT_TRUE(navigation.AddWaypoint(2, Vector3(5.0f, 0.0f, 0.0f)));
    ASSERT_TRUE(navigation.ConnectWaypoints(1, 2));
    ASSERT_TRUE(navigation.ConnectWaypoints(2, 3));

    const BotRoute route =
        navigation.BuildRoute(Vector3(-1.0f, 0.0f, 0.0f), Vector3(5.0f, 6.0f, 0.0f));
    ASSERT_TRUE(route.IsValid());
    EXPECT_TRUE(route.usedAuthoredGraph);
    EXPECT_FALSE(route.usedDirectFallback);
    ASSERT_EQ(route.waypoints.size(), static_cast<std::size_t>(4));
    EXPECT_EQ(route.waypoints[0], Vector3(0.0f, 0.0f, 0.0f));
    EXPECT_EQ(route.waypoints[1], Vector3(5.0f, 0.0f, 0.0f));
    EXPECT_EQ(route.waypoints[2], Vector3(5.0f, 5.0f, 0.0f));
    EXPECT_EQ(route.waypoints[3], Vector3(5.0f, 6.0f, 0.0f));

    // Authored edges are directed and the reverse trip exceeds the bounded
    // direct-route fallback.
    const BotRoute reverse =
        navigation.BuildRoute(Vector3(5.0f, 6.0f, 0.0f), Vector3(-1.0f, 0.0f, 0.0f));
    EXPECT_FALSE(reverse.IsValid());
}

TEST(BotNavigation, BoundsDirectFallbackDistance) {
    BotNavigationConfig config;
    config.enforceBounds = true;
    config.worldBounds = Bounds{Vector3(0.0f, 0.0f, 0.0f),
                                Vector3(10.0f, 10.0f, 10.0f)};
    config.maxDirectRouteLength = 5.0f;
    BotNavigation navigation(config);

    const BotRoute allowed =
        navigation.BuildRoute(Vector3(1.0f, 1.0f, 1.0f), Vector3(4.0f, 1.0f, 1.0f));
    ASSERT_TRUE(allowed.IsValid());
    EXPECT_TRUE(allowed.usedDirectFallback);

    EXPECT_FALSE(navigation
                     .BuildRoute(Vector3(1.0f, 1.0f, 1.0f),
                                 Vector3(9.0f, 1.0f, 1.0f))
                     .IsValid());
    EXPECT_FALSE(navigation
                     .BuildRoute(Vector3(1.0f, 1.0f, 1.0f),
                                 Vector3(11.0f, 1.0f, 1.0f))
                     .IsValid());
}

TEST(BotNavigation, EndpointSnapDistanceIsIndependentOfAuthoredEdgeLength) {
    BotNavigationConfig config;
    config.maxEdgeLength = 1000.0f;
    config.maxEndpointSnapDistance = 2.0f;
    config.maxDirectRouteLength = 0.0f;
    BotNavigation navigation(config);

    ASSERT_TRUE(navigation.AddWaypoint(1, Vector3(0.0f, 0.0f, 0.0f)));
    ASSERT_TRUE(navigation.AddWaypoint(2, Vector3(100.0f, 0.0f, 0.0f)));
    ASSERT_TRUE(navigation.ConnectWaypoints(1, 2));

    const BotRoute nearEndpoints = navigation.BuildRoute(
        Vector3(-1.0f, 0.0f, 0.0f), Vector3(101.0f, 0.0f, 0.0f), false);
    ASSERT_TRUE(nearEndpoints.IsValid());
    EXPECT_TRUE(nearEndpoints.usedAuthoredGraph);

    // The reviewed 100-unit edge remains valid, but neither endpoint may
    // attach through an un-authored chord longer than two units.
    EXPECT_FALSE(navigation
                     .BuildRoute(Vector3(-3.0f, 0.0f, 0.0f),
                                 Vector3(101.0f, 0.0f, 0.0f), false)
                     .IsValid());
    EXPECT_FALSE(navigation
                     .BuildRoute(Vector3(-1.0f, 0.0f, 0.0f),
                                 Vector3(103.0f, 0.0f, 0.0f), false)
                     .IsValid());

    BotNavigationConfig invalid = config;
    invalid.maxEndpointSnapDistance =
        std::numeric_limits<float>::infinity();
    EXPECT_FALSE(navigation.Configure(invalid));
    EXPECT_NEAR(navigation.GetConfig().maxEndpointSnapDistance, 2.0f, 0.0001f);

    invalid.maxEndpointSnapDistance = 0.0f;
    EXPECT_FALSE(navigation.Configure(invalid));
    invalid.maxEndpointSnapDistance = -1.0f;
    EXPECT_FALSE(navigation.Configure(invalid));
    EXPECT_NEAR(navigation.GetConfig().maxEndpointSnapDistance, 2.0f, 0.0001f);
}

TEST(BotNavigation, AdvanceConsumesOnlyTheDistanceBudget) {
    BotNavigation navigation;
    BotRoute route;
    route.waypoints.push_back(Vector3(3.0f, 0.0f, 0.0f));
    route.waypoints.push_back(Vector3(3.0f, 4.0f, 0.0f));

    std::size_t cursor = 0;
    Vector3 position = navigation.AdvanceAlongRoute(Vector3::Zero(), route, cursor, 2.0f);
    EXPECT_NEAR(position.x, 2.0f, 0.0001f);
    EXPECT_NEAR(position.y, 0.0f, 0.0001f);
    EXPECT_EQ(cursor, static_cast<std::size_t>(0));

    position = navigation.AdvanceAlongRoute(position, route, cursor, 2.0f);
    EXPECT_NEAR(position.x, 3.0f, 0.0001f);
    EXPECT_NEAR(position.y, 1.0f, 0.0001f);
    EXPECT_EQ(cursor, static_cast<std::size_t>(1));

    const Vector3 unchanged = navigation.AdvanceAlongRoute(
        position, route, cursor, std::numeric_limits<float>::infinity());
    EXPECT_EQ(unchanged, position);
}

TEST(BotNavigation, ReplaceGraphIsTransactionalAndRejectsDuplicateArcs) {
    BotNavigationConfig config;
    config.maxEdgeLength = 10.0f;
    config.maxEndpointSnapDistance = 2.0f;
    config.maxDirectRouteLength = 0.0f;
    BotNavigation navigation(config);

    BotNavigationGraph original;
    original.nodes = {
        BotWaypointNode{1, Vector3(0.0f, 0.0f, 0.0f)},
        BotWaypointNode{2, Vector3(5.0f, 0.0f, 0.0f)}
    };
    original.edges = {BotWaypointEdge{1, 2, false}};
    ASSERT_TRUE(navigation.ReplaceGraph(original));

    BotNavigationGraph invalid = original;
    invalid.edges = {
        BotWaypointEdge{1, 2, true},
        // The bidirectional row already owns this directed arc.
        BotWaypointEdge{2, 1, false}
    };
    EXPECT_FALSE(navigation.ReplaceGraph(invalid));

    const BotRoute preserved = navigation.BuildRoute(
        Vector3(-1.0f, 0.0f, 0.0f), Vector3(6.0f, 0.0f, 0.0f), false);
    ASSERT_TRUE(preserved.IsValid());
    EXPECT_TRUE(preserved.usedAuthoredGraph);

    EXPECT_TRUE(navigation.ReplaceGraph(BotNavigationGraph{}));
    EXPECT_EQ(navigation.GetWaypointCount(), static_cast<std::size_t>(0));
    EXPECT_FALSE(navigation
                     .BuildRoute(Vector3(-1.0f, 0.0f, 0.0f),
                                 Vector3(6.0f, 0.0f, 0.0f), false)
                     .IsValid());
}

RS2V_TEST_MAIN()
