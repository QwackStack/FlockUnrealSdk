// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Analytics/FlockAnalyticsConfig.h"

#include "Config/FlockConfig.h"

FFlockAnalyticsConfig FFlockAnalyticsConfig::FromSettings(const UFlockConfig& Settings)
{
	FFlockAnalyticsConfig Config;
	Config.bEnabled = Settings.bAnalyticsEnabled;
	Config.bRequireExplicitConsent = Settings.bAnalyticsRequireExplicitConsent;
	Config.bAutoStartSession = Settings.bAnalyticsAutoStartSession;
	Config.bAutoEndSessionOnQuit = Settings.bAnalyticsAutoEndOnQuit;
	Config.SessionTimeoutSeconds = Settings.AnalyticsSessionTimeout;
	Config.HeartbeatIntervalSeconds = Settings.AnalyticsHeartbeatInterval;
	Config.BounceThresholdSeconds = Settings.AnalyticsBounceThreshold;
	Config.bPersistSessionOnDisk = Settings.bAnalyticsPersistSession;
	Config.bTrackFps = Settings.bAnalyticsTrackFps;
	Config.FpsSampleIntervalSeconds = Settings.AnalyticsFpsSampleInterval;
	Config.bCacheFailedEvents = Settings.bAnalyticsCacheFailedEvents;
	Config.MaxCachedEvents = Settings.AnalyticsMaxCachedEvents;
	Config.CacheFlushBatchSize = Settings.AnalyticsCacheFlushBatchSize;
	Config.EventBufferFlushIntervalSeconds = Settings.AnalyticsEventBufferFlushInterval;
	return Config;
}
