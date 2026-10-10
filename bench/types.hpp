/*
Complete typed models for the service and earthquake fixtures. Explicit field mappings also work without compiler reflection support.
*/
#pragma once

#include <soml.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct service_server_tls {
	std::string certificate{};
	std::string key{};
	std::string min_version{};
	std::vector<std::string> ciphers{};
};

template<>
inline constexpr auto soml::fields<service_server_tls> = std::tuple{
	soml::field{"certificate", &service_server_tls::certificate},
	soml::field{"key", &service_server_tls::key},
	soml::field{"min-version", &service_server_tls::min_version},
	soml::field{"ciphers", &service_server_tls::ciphers},
};

struct service_server {
	std::string host{};
	std::int64_t port{};
	std::chrono::milliseconds keep_alive{};
	std::chrono::milliseconds read_timeout{};
	std::chrono::milliseconds write_timeout{};
	std::int64_t max_header_size{};
	service_server_tls tls{};
};

template<>
inline constexpr auto soml::fields<service_server> = std::tuple{
	soml::field{"host", &service_server::host},
	soml::field{"port", &service_server::port},
	soml::field{"keep-alive", &service_server::keep_alive},
	soml::field{"read-timeout", &service_server::read_timeout},
	soml::field{"write-timeout", &service_server::write_timeout},
	soml::field{"max-header-size", &service_server::max_header_size},
	soml::field{"tls", &service_server::tls},
};

struct service_database_pool {
	std::int64_t min{};
	std::int64_t max{};
	std::chrono::milliseconds idle_timeout{};
	std::chrono::milliseconds acquire_timeout{};
};

template<>
inline constexpr auto soml::fields<service_database_pool> = std::tuple{
	soml::field{"min", &service_database_pool::min},
	soml::field{"max", &service_database_pool::max},
	soml::field{"idle-timeout", &service_database_pool::idle_timeout},
	soml::field{"acquire-timeout", &service_database_pool::acquire_timeout},
};

struct service_database {
	std::string primary{};
	std::vector<std::string> replicas{};
	service_database_pool pool{};
	std::string setup{};
};

template<>
inline constexpr auto soml::fields<service_database> = std::tuple{
	soml::field{"primary", &service_database::primary},
	soml::field{"replicas", &service_database::replicas},
	soml::field{"pool", &service_database::pool},
	soml::field{"setup", &service_database::setup},
};

struct service_cache {
	std::string url{};
	std::chrono::milliseconds ttl{};
	std::int64_t max_entries{};
	std::string eviction{};
};

template<>
inline constexpr auto soml::fields<service_cache> = std::tuple{
	soml::field{"url", &service_cache::url},
	soml::field{"ttl", &service_cache::ttl},
	soml::field{"max-entries", &service_cache::max_entries},
	soml::field{"eviction", &service_cache::eviction},
};

struct service_queue_exchanges_item {
	std::string name{};
	std::string type{};
	bool durable{};
};

template<>
inline constexpr auto soml::fields<service_queue_exchanges_item> = std::tuple{
	soml::field{"name", &service_queue_exchanges_item::name},
	soml::field{"type", &service_queue_exchanges_item::type},
	soml::field{"durable", &service_queue_exchanges_item::durable},
};

struct service_queue_retry {
	std::int64_t attempts{};
	std::chrono::milliseconds initial_delay{};
	std::chrono::milliseconds max_delay{};
	double multiplier{};
};

template<>
inline constexpr auto soml::fields<service_queue_retry> = std::tuple{
	soml::field{"attempts", &service_queue_retry::attempts},
	soml::field{"initial-delay", &service_queue_retry::initial_delay},
	soml::field{"max-delay", &service_queue_retry::max_delay},
	soml::field{"multiplier", &service_queue_retry::multiplier},
};

struct service_queue {
	std::string broker{};
	std::vector<service_queue_exchanges_item> exchanges{};
	service_queue_retry retry{};
};

template<>
inline constexpr auto soml::fields<service_queue> = std::tuple{
	soml::field{"broker", &service_queue::broker},
	soml::field{"exchanges", &service_queue::exchanges},
	soml::field{"retry", &service_queue::retry},
};

struct service_payments_fees {
	double percentage{};
	std::int64_t fixed{};
};

template<>
inline constexpr auto soml::fields<service_payments_fees> = std::tuple{
	soml::field{"percentage", &service_payments_fees::percentage},
	soml::field{"fixed", &service_payments_fees::fixed},
};

struct service_payments_webhooks {
	std::string path{};
	std::chrono::milliseconds tolerance{};
	std::vector<std::string> events{};
};

template<>
inline constexpr auto soml::fields<service_payments_webhooks> = std::tuple{
	soml::field{"path", &service_payments_webhooks::path},
	soml::field{"tolerance", &service_payments_webhooks::tolerance},
	soml::field{"events", &service_payments_webhooks::events},
};

struct service_payments {
	std::string provider{};
	std::string currency{};
	std::string capture{};
	std::int64_t review_threshold{};
	service_payments_fees fees{};
	service_payments_webhooks webhooks{};
};

template<>
inline constexpr auto soml::fields<service_payments> = std::tuple{
	soml::field{"provider", &service_payments::provider},
	soml::field{"currency", &service_payments::currency},
	soml::field{"capture", &service_payments::capture},
	soml::field{"review-threshold", &service_payments::review_threshold},
	soml::field{"fees", &service_payments::fees},
	soml::field{"webhooks", &service_payments::webhooks},
};

struct service_limits {
	std::int64_t requests_per_minute{};
	std::int64_t burst{};
	std::int64_t max_body_size{};
	std::int64_t max_items_per_order{};
	double cpu{};
	std::int64_t memory_mb{};
};

template<>
inline constexpr auto soml::fields<service_limits> = std::tuple{
	soml::field{"requests-per-minute", &service_limits::requests_per_minute},
	soml::field{"burst", &service_limits::burst},
	soml::field{"max-body-size", &service_limits::max_body_size},
	soml::field{"max-items-per-order", &service_limits::max_items_per_order},
	soml::field{"cpu", &service_limits::cpu},
	soml::field{"memory-mb", &service_limits::memory_mb},
};

struct service_features {
	bool express_checkout{};
	bool gift_cards{};
	bool buy_now_pay_later{};
	bool address_autocomplete{};
	bool new_tax_engine{};
};

template<>
inline constexpr auto soml::fields<service_features> = std::tuple{
	soml::field{"express-checkout", &service_features::express_checkout},
	soml::field{"gift-cards", &service_features::gift_cards},
	soml::field{"buy-now-pay-later", &service_features::buy_now_pay_later},
	soml::field{"address-autocomplete", &service_features::address_autocomplete},
	soml::field{"new-tax-engine", &service_features::new_tax_engine},
};

struct service_logging_file {
	std::string path{};
	std::int64_t mode{};
	std::int64_t max_size_mb{};
	std::int64_t max_files{};
};

template<>
inline constexpr auto soml::fields<service_logging_file> = std::tuple{
	soml::field{"path", &service_logging_file::path},
	soml::field{"mode", &service_logging_file::mode},
	soml::field{"max-size-mb", &service_logging_file::max_size_mb},
	soml::field{"max-files", &service_logging_file::max_files},
};

struct service_logging {
	std::string level{};
	std::string format{};
	double sample_rate{};
	std::vector<std::string> redact{};
	service_logging_file file{};
};

template<>
inline constexpr auto soml::fields<service_logging> = std::tuple{
	soml::field{"level", &service_logging::level},
	soml::field{"format", &service_logging::format},
	soml::field{"sample-rate", &service_logging::sample_rate},
	soml::field{"redact", &service_logging::redact},
	soml::field{"file", &service_logging::file},
};

struct service_telemetry_attributes {
	std::string team{};
	std::string service_tier{};
};

template<>
inline constexpr auto soml::fields<service_telemetry_attributes> = std::tuple{
	soml::field{"team", &service_telemetry_attributes::team},
	soml::field{"service.tier", &service_telemetry_attributes::service_tier},
};

struct service_telemetry_histograms {
	std::vector<double> request_duration{};
};

template<>
inline constexpr auto soml::fields<service_telemetry_histograms> = std::tuple{
	soml::field{"request-duration", &service_telemetry_histograms::request_duration},
};

struct service_telemetry {
	std::string endpoint{};
	std::chrono::milliseconds interval{};
	service_telemetry_attributes attributes{};
	service_telemetry_histograms histograms{};
};

template<>
inline constexpr auto soml::fields<service_telemetry> = std::tuple{
	soml::field{"endpoint", &service_telemetry::endpoint},
	soml::field{"interval", &service_telemetry::interval},
	soml::field{"attributes", &service_telemetry::attributes},
	soml::field{"histograms", &service_telemetry::histograms},
};

struct service_maintenance {
	bool enabled{};
	std::string message{};
};

template<>
inline constexpr auto soml::fields<service_maintenance> = std::tuple{
	soml::field{"enabled", &service_maintenance::enabled},
	soml::field{"message", &service_maintenance::message},
};

struct service_regions_item {
	std::string name{};
	std::int64_t weight{};
	bool primary{};
};

template<>
inline constexpr auto soml::fields<service_regions_item> = std::tuple{
	soml::field{"name", &service_regions_item::name},
	soml::field{"weight", &service_regions_item::weight},
	soml::field{"primary", &service_regions_item::primary},
};

struct service {
	std::string name{};
	std::string version{};
	std::string environment{};
	std::string region{};
	soml::instant deployed_at{};
	service_server server{};
	service_database database{};
	service_cache cache{};
	service_queue queue{};
	service_payments payments{};
	service_limits limits{};
	service_features features{};
	service_logging logging{};
	service_telemetry telemetry{};
	service_maintenance maintenance{};
	std::vector<service_regions_item> regions{};
};

template<>
inline constexpr auto soml::fields<service> = std::tuple{
	soml::field{"name", &service::name},
	soml::field{"version", &service::version},
	soml::field{"environment", &service::environment},
	soml::field{"region", &service::region},
	soml::field{"deployed-at", &service::deployed_at},
	soml::field{"server", &service::server},
	soml::field{"database", &service::database},
	soml::field{"cache", &service::cache},
	soml::field{"queue", &service::queue},
	soml::field{"payments", &service::payments},
	soml::field{"limits", &service::limits},
	soml::field{"features", &service::features},
	soml::field{"logging", &service::logging},
	soml::field{"telemetry", &service::telemetry},
	soml::field{"maintenance", &service::maintenance},
	soml::field{"regions", &service::regions},
};

struct earthquakes_metadata {
	soml::instant generated{};
	std::string url{};
	std::string title{};
	std::int64_t status{};
	std::string api{};
	std::int64_t count{};
};

template<>
inline constexpr auto soml::fields<earthquakes_metadata> = std::tuple{
	soml::field{"generated", &earthquakes_metadata::generated},
	soml::field{"url", &earthquakes_metadata::url},
	soml::field{"title", &earthquakes_metadata::title},
	soml::field{"status", &earthquakes_metadata::status},
	soml::field{"api", &earthquakes_metadata::api},
	soml::field{"count", &earthquakes_metadata::count},
};

struct earthquakes_features_item_properties {
	double mag{};
	std::string place{};
	soml::instant time{};
	soml::instant updated{};
	std::string url{};
	std::string detail{};
	std::string status{};
	std::int64_t tsunami{};
	std::int64_t sig{};
	std::string net{};
	std::string code{};
	std::string ids{};
	std::string sources{};
	std::string types{};
	std::int64_t nst{};
	double dmin{};
	double rms{};
	double gap{};
	std::string mag_type{};
	std::string type{};
	std::string title{};
	std::optional<std::int64_t> felt{};
	std::optional<double> cdi{};
	std::optional<double> mmi{};
	std::optional<std::string> alert{};
};

template<>
inline constexpr auto soml::fields<earthquakes_features_item_properties> = std::tuple{
	soml::field{"mag", &earthquakes_features_item_properties::mag},
	soml::field{"place", &earthquakes_features_item_properties::place},
	soml::field{"time", &earthquakes_features_item_properties::time},
	soml::field{"updated", &earthquakes_features_item_properties::updated},
	soml::field{"url", &earthquakes_features_item_properties::url},
	soml::field{"detail", &earthquakes_features_item_properties::detail},
	soml::field{"status", &earthquakes_features_item_properties::status},
	soml::field{"tsunami", &earthquakes_features_item_properties::tsunami},
	soml::field{"sig", &earthquakes_features_item_properties::sig},
	soml::field{"net", &earthquakes_features_item_properties::net},
	soml::field{"code", &earthquakes_features_item_properties::code},
	soml::field{"ids", &earthquakes_features_item_properties::ids},
	soml::field{"sources", &earthquakes_features_item_properties::sources},
	soml::field{"types", &earthquakes_features_item_properties::types},
	soml::field{"nst", &earthquakes_features_item_properties::nst},
	soml::field{"dmin", &earthquakes_features_item_properties::dmin},
	soml::field{"rms", &earthquakes_features_item_properties::rms},
	soml::field{"gap", &earthquakes_features_item_properties::gap},
	soml::field{"magType", &earthquakes_features_item_properties::mag_type},
	soml::field{"type", &earthquakes_features_item_properties::type},
	soml::field{"title", &earthquakes_features_item_properties::title},
	soml::field{"felt", &earthquakes_features_item_properties::felt},
	soml::field{"cdi", &earthquakes_features_item_properties::cdi},
	soml::field{"mmi", &earthquakes_features_item_properties::mmi},
	soml::field{"alert", &earthquakes_features_item_properties::alert},
};

struct earthquakes_features_item_geometry {
	std::string type{};
	std::vector<double> coordinates{};
};

template<>
inline constexpr auto soml::fields<earthquakes_features_item_geometry> = std::tuple{
	soml::field{"type", &earthquakes_features_item_geometry::type},
	soml::field{"coordinates", &earthquakes_features_item_geometry::coordinates},
};

struct earthquakes_features_item {
	std::string type{};
	earthquakes_features_item_properties properties{};
	earthquakes_features_item_geometry geometry{};
	std::string id{};
};

template<>
inline constexpr auto soml::fields<earthquakes_features_item> = std::tuple{
	soml::field{"type", &earthquakes_features_item::type},
	soml::field{"properties", &earthquakes_features_item::properties},
	soml::field{"geometry", &earthquakes_features_item::geometry},
	soml::field{"id", &earthquakes_features_item::id},
};

struct earthquakes {
	std::string type{};
	earthquakes_metadata metadata{};
	std::vector<double> bbox{};
	std::vector<earthquakes_features_item> features{};
};

template<>
inline constexpr auto soml::fields<earthquakes> = std::tuple{
	soml::field{"type", &earthquakes::type},
	soml::field{"metadata", &earthquakes::metadata},
	soml::field{"bbox", &earthquakes::bbox},
	soml::field{"features", &earthquakes::features},
};
