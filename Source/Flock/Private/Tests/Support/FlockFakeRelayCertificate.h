// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

/**
 * A self-signed certificate for relay.test, localhost and 127.0.0.1 (made with openssl on 2026-10-25, lasting 100 years) and its
 * key, for the fake relay's TLS side alone. No platform trusts it: a client lets it through only once a test trusts it
 * (FFlockRelayTls::SetTrustedCertificateForTesting). Include only from guarded test files.
 */
namespace FlockFakeRelayCertificate
{
	inline const TCHAR* Certificate =
		TEXT("-----BEGIN CERTIFICATE-----\n")
		TEXT("MIIDNTCCAh2gAwIBAgIUV2qzvBMLUcx3fb9tgPnsdzV/400wDQYJKoZIhvcNAQEL\n")
		TEXT("BQAwFTETMBEGA1UEAwwKcmVsYXkudGVzdDAgFw0yNjEwMjUwNzA5NDJaGA8yMTI2\n")
		TEXT("MTAwMTA3MDk0MlowFTETMBEGA1UEAwwKcmVsYXkudGVzdDCCASIwDQYJKoZIhvcN\n")
		TEXT("AQEBBQADggEPADCCAQoCggEBANDWzfb7iKamnL4R9VhOXWeIhUZCYzhD5r8NsBbd\n")
		TEXT("bpXmsu9hCBVqgmhX0sjpWMmPhJLge12TEqYEGjJHfg2XmmxvwCE+NYUyVx1rTtVp\n")
		TEXT("qWK0aAxFM0MhHkLnoKI47hdqHxLNqHq0o3n5G+3fWjkNlEdUzfxNL/+55Pt4feMD\n")
		TEXT("CniqZ+rYuh4dU8dvcaP85swT161Sq3OjyLWBFMXmYV/hpqIqkE5Iqvr7pg0fLF/2\n")
		TEXT("VptsYKEIJ+U+0k1ErQBUQa/VWQHIhdPbsnYeY8ATz+hVLrjeEaTxyO5/xRFTDw3Q\n")
		TEXT("NcbMyMxyPUV9ycd1hwXIMiIOlmfuGucvpR2gnPRGRdz4txcCAwEAAaN7MHkwHQYD\n")
		TEXT("VR0OBBYEFAnf5npR17O2La4IYdNRf+o73pIDMB8GA1UdIwQYMBaAFAnf5npR17O2\n")
		TEXT("La4IYdNRf+o73pIDMA8GA1UdEwEB/wQFMAMBAf8wJgYDVR0RBB8wHYIKcmVsYXku\n")
		TEXT("dGVzdIIJbG9jYWxob3N0hwR/AAABMA0GCSqGSIb3DQEBCwUAA4IBAQA6pqhIEeoz\n")
		TEXT("9p6CD5QczyZgFgBucqjd5faoGmdSKuIhL56o/NDY9bLhcENELcxgt98Cmuaqfhna\n")
		TEXT("5PB04nkCdyfzMQwDiy87ypuC1WsBE1CAqAsCa1tkEJ8YheKQiJvHcstdMjADbGgD\n")
		TEXT("a/hAxzJHWjpVyI1pR178QGq+CUzo8NkgYq9P9/2RtyVvVas5oZCSpWnU9N8+Bzdg\n")
		TEXT("QopoKPREEh4/lYFVRWdNFNzB7bctmcpqMvfqvG9G0TK2VeYkx8mxCgeEBylHeoGC\n")
		TEXT("XDHoBoBQaR+uJMVMmWUM6fC0sCtKRDPoy/jtImuUe96AhF/p4ndcajsT/9SU1Kzn\n")
		TEXT("LjFp/xSelzCC\n")
		TEXT("-----END CERTIFICATE-----\n");

	inline const TCHAR* Key =
		TEXT("-----BEGIN PRIVATE KEY-----\n")
		TEXT("MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQDQ1s32+4imppy+\n")
		TEXT("EfVYTl1niIVGQmM4Q+a/DbAW3W6V5rLvYQgVaoJoV9LI6VjJj4SS4HtdkxKmBBoy\n")
		TEXT("R34Nl5psb8AhPjWFMlcda07VaalitGgMRTNDIR5C56CiOO4Xah8Szah6tKN5+Rvt\n")
		TEXT("31o5DZRHVM38TS//ueT7eH3jAwp4qmfq2LoeHVPHb3Gj/ObME9etUqtzo8i1gRTF\n")
		TEXT("5mFf4aaiKpBOSKr6+6YNHyxf9labbGChCCflPtJNRK0AVEGv1VkByIXT27J2HmPA\n")
		TEXT("E8/oVS643hGk8cjuf8URUw8N0DXGzMjMcj1FfcnHdYcFyDIiDpZn7hrnL6UdoJz0\n")
		TEXT("RkXc+LcXAgMBAAECggEALAkPNwQE5Xb+WAm7XRpxEZK034LCHywaLLD7L2cTmWIi\n")
		TEXT("2tzv8mwaAVx7YsfpY97ySnIxMSMA7Kayy2VNzBnqgvDxZ1ERwftCaT6mSdPU1q5J\n")
		TEXT("vkSdMYt7MQpcUbiFY8SJttJMs0sBqYeAfyD4ik01tfDI2mmANIEDKo9WpBaRD64l\n")
		TEXT("U/bwNJfl1RWk6S+01p13AzHwXu8Wv/7WcekHFb66a/Y7FGPw6pdskPHJwc3mofb2\n")
		TEXT("bsgCeiYgorGllJoc7S9GoHLFKt+38M1uZSM1xdsgIpE3IlunDitbXUCS8+LR0OTv\n")
		TEXT("VePRzvF8jXSeP29tCryWC62D0FnDw2883WGcjAH/WQKBgQDwVKv1Z1OHGgZ17nCw\n")
		TEXT("PErHE7z30P/pxGX1xZGDtbl4ZeHHJ7RmZIv1jxIyDJ1BNVX1/lTUhRupie0OhLih\n")
		TEXT("T2qycEWNDQ9gzspQhVOUbPVgapnb/XT2WAQ9gSsFajdSaFIZjVx2UaJ3k10DK0cq\n")
		TEXT("Wxn2WOhU+DnqmZuKkuT2VaWA+wKBgQDedIJ2glwozbpPHONyq4fTb5uG0z3lOGYM\n")
		TEXT("JbTmlFsyKyepcUp+rECwIjIzVdFzzkfol9+iGtLDhqq8lOtGbmGAsH834UqSXv/D\n")
		TEXT("YBFpWWHhgOhsM00mSb5SVDEABBSt35+G5ti5P5BXId0g8kcmFNesoeYMhFrkhvcd\n")
		TEXT("5ECA+r7flQKBgQDRGRoz/Z5S4XXVjYtGCm8VHjs/cfv2rAX5+9dLPQUNl7cEy6rN\n")
		TEXT("m5WaJMyz9v2rCnir+ohcDyOuMm6qPxdUYgB3F++rdT14APNNc3LwpIwpQxZyrObi\n")
		TEXT("nlmyGXwQzgUsEc+8o51a5FPyragzZagZVgIUr+4UX5QkZVlLC9th58qDmQKBgBFZ\n")
		TEXT("Nr91RSiteqUm1JohxHL51kA6gy44KsUxv5zn3wDXmr09J5kfZ+AUmqOkQrRsVvFN\n")
		TEXT("vtQOkXdMLk6MspiIjTQTD6jU+hLWM84R1OnJXbfPbNDbY13kDGKxTCz7JH223eMQ\n")
		TEXT("Z8Bys5gH/Q3PtzrCD1vYOG+rdscxu13qahlk9LPNAoGAPHIGuDYQlHPg5x0UwXw2\n")
		TEXT("Fhnrs7OnpyxZxPdSZA5y24c2uYCd0bQbRiz4phduZ+yB0Kd2nuXa/raZgAoLgvFi\n")
		TEXT("SiSJC781ws49ZpQWig0uwQC18N+sCMMBVfJfwvQYuJht2xbnxgbsiZuqqbIhKVkK\n")
		TEXT("tXWQRcPy4fDYGPBL8Y6axik=\n")
		TEXT("-----END PRIVATE KEY-----\n");
}

#endif
