// Copyright (c) 2019, QuantStack and Mamba Contributors
//
// Distributed under the terms of the BSD 3-Clause License.
//
// The full license is in the file LICENSE, distributed with this software.

#include <regex>
#include <stdexcept>

#include "mamba/core/channel_context.hpp"
#include "mamba/core/context.hpp"
#include "mamba/core/output.hpp"
#include "mamba/core/package_cache.hpp"
#include "mamba/core/subdirdata.hpp"
#include "mamba/core/thread_utils.hpp"
#include "mamba/fs/filesystem.hpp"
#include "mamba/specs/channel.hpp"
#include "mamba/util/cryptography.hpp"
#include "mamba/util/json.hpp"
#include "mamba/util/string.hpp"
#include "mamba/util/url_manip.hpp"

namespace mamba
{
    /*******************
     * MSubdirMetadata *
     *******************/

    namespace
    {
#ifdef _WIN32
        std::chrono::system_clock::time_point filetime_to_unix(const fs::file_time_type& filetime)
        {
            // windows filetime is in 100ns intervals since 1601-01-01
            static constexpr auto epoch_offset = std::chrono::seconds(11644473600ULL);
            return std::chrono::system_clock::time_point(
                std::chrono::duration_cast<std::chrono::system_clock::duration>(
                    filetime.time_since_epoch() - epoch_offset
                )
            );
        }
#endif

        // parse json at the beginning of the stream such as
        // "_url": "https://conda.anaconda.org/conda-forge/linux-64",
        // "_etag": "W/\"6092e6a2b6cec6ea5aade4e177c3edda-8\"",
        // "_mod": "Sat, 04 Apr 2020 03:29:49 GMT",
        // "_cache_control": "public, max-age=1200"
        std::string extract_subjson(std::ifstream& s)
        {
            char next = {};
            std::string result = {};
            bool escaped = false;
            int i = 0, N = 4;
            std::size_t idx = 0;
            std::size_t prev_keystart = 0;
            bool in_key = false;
            std::string key = "";

            while (s.get(next))
            {
                idx++;
                if (next == '"')
                {
                    if (!escaped)
                    {
                        if ((i / 2) % 2 == 0)
                        {
                            in_key = !in_key;
                            if (in_key)
                            {
                                prev_keystart = idx + 1;
                            }
                            else
                            {
                                if (key != "_mod" && key != "_etag" && key != "_cache_control"
                                    && key != "_url")
                                {
                                    // bail out
                                    auto last_comma = result.find_last_of(",", prev_keystart - 2);
                                    if (last_comma != std::string::npos && last_comma > 0)
                                    {
                                        result = result.substr(0, last_comma);
                                        result.push_back('}');
                                        return result;
                                    }
                                    else
                                    {
                                        return std::string();
                                    }
                                }
                                key.clear();
                            }
                        }
                        i++;
                    }

                    // 4 keys == 4 ticks
                    if (i == 4 * N)
                    {
                        result += "\"}";
                        return result;
                    }
                }

                if (in_key && next != '"')
                {
                    key.push_back(next);
                }

                escaped = (!escaped && next == '\\');
                result.push_back(next);
            }
            return std::string();
        }
    }

    /*******************
     * MSubdirMetadata *
     *******************/

    void to_json(nlohmann::json& j, const SubdirMetadata::CheckedAt& ca)
    {
        j["value"] = ca.value;
        j["last_checked"] = timestamp(ca.last_checked);
    }

    void from_json(const nlohmann::json& j, SubdirMetadata::CheckedAt& ca)
    {
        int err_code = 0;
        ca.value = j["value"].get<bool>();
        ca.last_checked = parse_utc_timestamp(j["last_checked"].get<std::string>(), err_code);
    }

    void to_json(nlohmann::json& j, const SubdirMetadata& data)
    {
        j["url"] = data.m_http.url;
        j["etag"] = data.m_http.etag;
        j["mod"] = data.m_http.last_modified;
        j["cache_control"] = data.m_http.cache_control;
        j["size"] = data.m_stored_file_size;

        auto nsecs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            data.m_stored_mtime.time_since_epoch()
        );
        j["mtime_ns"] = nsecs.count();
        j["has_zst"] = data.m_has_zst;
    }

    void from_json(const nlohmann::json& j, SubdirMetadata& data)
    {
        data.m_http.url = j["url"].get<std::string>();
        data.m_http.etag = j["etag"].get<std::string>();
        data.m_http.last_modified = j["mod"].get<std::string>();
        data.m_http.cache_control = j["cache_control"].get<std::string>();
        data.m_stored_file_size = j["size"].get<std::size_t>();

        using time_type = decltype(data.m_stored_mtime);
        data.m_stored_mtime = time_type(std::chrono::duration_cast<time_type::duration>(
            std::chrono::nanoseconds(j["mtime_ns"].get<std::size_t>())
        ));
        util::deserialize_maybe_missing(j, "has_zst", data.m_has_zst);
    }

    auto SubdirMetadata::read(const fs::u8path& file) -> expected_subdir_metadata
    {
        fs::u8path state_file = file;
        state_file.replace_extension(".state.json");
        if (fs::is_regular_file(state_file))
        {
            return from_state_file(state_file, file);
        }
        else
        {
            return from_repodata_file(file);
        }
    }

    void SubdirMetadata::write(const fs::u8path& file)
    {
        nlohmann::json j = *this;
        std::ofstream out = open_ofstream(file);
        out << j.dump(4);
    }

    bool SubdirMetadata::check_valid_metadata(const fs::u8path& file)
    {
        if (const auto new_size = fs::file_size(file); new_size != m_stored_file_size)
        {
            LOG_INFO << "File size changed, expected " << m_stored_file_size << " but got "
                     << new_size << "; invalidating metadata";
            return false;
        }
#ifndef _WIN32
        bool last_write_time_valid = fs::last_write_time(file) == m_stored_mtime;
#else
        bool last_write_time_valid = filetime_to_unix(fs::last_write_time(file)) == m_stored_mtime;
#endif
        if (!last_write_time_valid)
        {
            LOG_INFO << "File mtime changed, invalidating metadata";
        }
        return last_write_time_valid;
    }

    const std::string& SubdirMetadata::url() const
    {
        return m_http.url;
    }

    const std::string& SubdirMetadata::etag() const
    {
        return m_http.etag;
    }

    const std::string& SubdirMetadata::last_modified() const
    {
        return m_http.last_modified;
    }

    const std::string& SubdirMetadata::cache_control() const
    {
        return m_http.cache_control;
    }

    bool SubdirMetadata::has_zst() const
    {
        return m_has_zst.has_value() && m_has_zst.value().value && !m_has_zst.value().has_expired();
    }

    void SubdirMetadata::store_http_metadata(HttpMetadata data)
    {
        m_http = std::move(data);
    }

    void SubdirMetadata::store_file_metadata(const fs::u8path& file)
    {
#ifndef _WIN32
        m_stored_mtime = fs::last_write_time(file);
#else
        // convert windows filetime to unix timestamp
        m_stored_mtime = filetime_to_unix(fs::last_write_time(file));
#endif
        m_stored_file_size = fs::file_size(file);
    }

    void SubdirMetadata::set_zst(bool value)
    {
        m_has_zst = { value, std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) };
    }

    auto
    SubdirMetadata::from_state_file(const fs::u8path& state_file, const fs::u8path& repodata_file)
        -> expected_subdir_metadata
    {
        std::ifstream infile = open_ifstream(state_file);
        nlohmann::json j = nlohmann::json::parse(infile);
        SubdirMetadata m;
        try
        {
            m = j.get<SubdirMetadata>();
        }
        catch (const std::exception& e)
        {
            LOG_WARNING << "Could not parse state file: " << e.what();
            std::error_code ec;
            fs::remove(state_file, ec);
            if (ec)
            {
                LOG_WARNING << "Could not remove state file " << state_file << ": " << ec.message();
            }
            return make_unexpected(
                fmt::format("File: {}: Could not load cache state: {}", state_file, e.what()),
                mamba_error_code::cache_not_loaded
            );
        }

        if (!m.check_valid_metadata(repodata_file))
        {
            LOG_WARNING << "Cache file " << repodata_file << " was modified by another program";
            return make_unexpected(
                fmt::format("File: {}: Cache file mtime mismatch", state_file),
                mamba_error_code::cache_not_loaded
            );
        }
        return m;
    }

    auto SubdirMetadata::from_repodata_file(const fs::u8path& repodata_file)
        -> expected_subdir_metadata
    {
        const std::string json = [](const fs::u8path& file) -> std::string
        {
            auto lock = LockFile(file);
            std::ifstream in_file = open_ifstream(file);
            return extract_subjson(in_file);
        }(repodata_file);

        try
        {
            nlohmann::json result = nlohmann::json::parse(json);
            SubdirMetadata m;
            m.m_http.url = result.value("_url", "");
            m.m_http.etag = result.value("_etag", "");
            m.m_http.last_modified = result.value("_mod", "");
            m.m_http.cache_control = result.value("_cache_control", "");
            return m;
        }
        catch (std::exception& e)
        {
            LOG_DEBUG << "Could not parse mod/etag header";
            return make_unexpected(
                fmt::format("File: {}: Could not parse mod/etag header ({})", repodata_file, e.what()),
                mamba_error_code::cache_not_loaded
            );
        }
    }

    bool SubdirMetadata::CheckedAt::has_expired() const
    {
        // difference in seconds, check every 14 days
        constexpr double expiration = 60 * 60 * 24 * 14;
        return std::difftime(std::time(nullptr), last_checked) > expiration;
    }

    /***************
     * MSubdirData *
     ***************/

    namespace
    {
        using file_duration = fs::file_time_type::duration;
        using file_time_point = fs::file_time_type::clock::time_point;

        template <typename T>
        std::vector<T> without_duplicates(std::vector<T>&& values)
        {
            const auto end_it = std::unique(values.begin(), values.end());
            values.erase(end_it, values.end());
            return values;
        }

        file_duration get_cache_age(const fs::u8path& cache_file, const file_time_point& ref)
        {
            try
            {
                fs::file_time_type last_write = fs::last_write_time(cache_file);
                file_duration tdiff = ref - last_write;
                return tdiff;
            }
            catch (std::exception&)
            {
                // could not open the file...
                return file_duration::max();
            }
        }

        bool is_valid(const file_duration& age)
        {
            return age != file_duration::max();
        }

        int get_max_age(const std::string& cache_control, int local_repodata_ttl)
        {
            int max_age = local_repodata_ttl;
            if (local_repodata_ttl == 1)
            {
                static std::regex max_age_re("max-age=(\\d+)");
                std::smatch max_age_match;
                bool matches = std::regex_search(cache_control, max_age_match, max_age_re);
                if (!matches)
                {
                    max_age = 0;
                }
                else
                {
                    max_age = std::stoi(max_age_match[1]);
                }
            }
            return max_age;
        }

        fs::u8path get_cache_dir(const fs::u8path& cache_path)
        {
            return cache_path / "cache";
        }

        const fs::u8path& replace_file(const fs::u8path& old_file, const fs::u8path& new_file)
        {
            if (fs::is_regular_file(old_file))
            {
                fs::remove(old_file);
            }
            fs::copy(new_file, old_file);
            return old_file;
        }
    }

    expected_t<SubdirData> SubdirData::create(
        Context& ctx,
        ChannelContext& channel_context,
        const specs::Channel& channel,
        const std::string& platform,
        MultiPackageCache& caches,
        const std::string& repodata_fn
    )
    {
        try
        {
            return SubdirData(ctx, channel_context, channel, platform, caches, repodata_fn);
        }
        catch (std::exception& e)
        {
            return make_unexpected(e.what(), mamba_error_code::subdirdata_not_loaded);
        }
        catch (...)
        {
            return make_unexpected(
                "Unknown error when trying to load subdir data "
                    + SubdirData::get_name(channel.id(), platform),
                mamba_error_code::unknown
            );
        }
    }

    bool SubdirData::is_noarch() const
    {
        return m_is_noarch;
    }

    bool SubdirData::is_loaded() const
    {
        return m_loaded;
    }

    void SubdirData::clear_cache()
    {
        if (fs::is_regular_file(m_json_fn))
        {
            fs::remove(m_json_fn);
        }
        if (fs::is_regular_file(m_solv_fn))
        {
            fs::remove(m_solv_fn);
        }
    }

    const std::string& SubdirData::name() const
    {
        return m_name;
    }

    const std::string& SubdirData::channel_id() const
    {
        return m_channel_id;
    }

    const std::string& SubdirData::platform() const
    {
        return m_platform;
    }

    const SubdirMetadata& SubdirData::metadata() const
    {
        return m_metadata;
    }

    expected_t<fs::u8path> SubdirData::valid_solv_cache() const
    {
        if (m_json_cache_valid && m_solv_cache_valid)
        {
            return (get_cache_dir(m_valid_cache_path) / m_solv_fn).string();
        }
        return make_unexpected("Cache not loaded", mamba_error_code::cache_not_loaded);
    }

    fs::u8path SubdirData::writable_solv_cache() const
    {
        return m_writable_pkgs_dir / "cache" / m_solv_fn;
    }

    expected_t<fs::u8path> SubdirData::valid_json_cache() const
    {
        if (m_json_cache_valid)
        {
            return (get_cache_dir(m_valid_cache_path) / m_json_fn).string();
        }
        return make_unexpected("Cache not loaded", mamba_error_code::cache_not_loaded);
    }

    expected_t<std::string> SubdirData::cache_path() const
    {
        // TODO invalidate solv cache on version updates!!
        if (m_json_cache_valid && m_solv_cache_valid)
        {
            return (get_cache_dir(m_valid_cache_path) / m_solv_fn).string();
        }
        else if (m_json_cache_valid)
        {
            return (get_cache_dir(m_valid_cache_path) / m_json_fn).string();
        }
        return make_unexpected("Cache not loaded", mamba_error_code::cache_not_loaded);
    }

    expected_t<void> SubdirData::download_indexes(
        std::vector<SubdirData>& subdirs,
        const Context& context,
        download::Monitor* check_monitor,
        download::Monitor* download_monitor
    )
    {
        download::MultiRequest check_requests;
        for (auto& subdir : subdirs)
        {
            if (!subdir.is_loaded())
            {
                download::MultiRequest check_list = subdir.build_check_requests();
                std::move(check_list.begin(), check_list.end(), std::back_inserter(check_requests));
            }
        }
        download::download(
            std::move(check_requests),
            context.mirrors,
            context.remote_fetch_params,
            context.authentication_info(),
            context.download_options(),
            check_monitor
        );

        if (is_sig_interrupted())
        {
            return make_unexpected("Interrupted by user", mamba_error_code::user_interrupted);
        }

        // TODO load local channels even when offline if (!ctx.offline)
        if (!context.offline)
        {
            download::MultiRequest index_requests;
            for (auto& subdir : subdirs)
            {
                if (!subdir.is_loaded())
                {
                    index_requests.push_back(subdir.build_index_request());
                }
            }

            try
            {
                download::download(
                    std::move(index_requests),
                    context.mirrors,
                    context.remote_fetch_params,
                    context.authentication_info(),
                    context.download_options(),
                    download_monitor
                );
            }
            catch (const std::runtime_error& e)
            {
                return make_unexpected(e.what(), mamba_error_code::repodata_not_loaded);
            }
        }

        return expected_t<void>();
    }

    std::string SubdirData::get_name(const std::string& channel_id, const std::string& platform)
    {
        return util::url_concat(channel_id, "/", platform);
    }

    SubdirData::SubdirData(
        Context& ctx,
        ChannelContext& channel_context,
        const specs::Channel& channel,
        const std::string& platform,
        MultiPackageCache& caches,
        const std::string& repodata_fn
    )
        : m_valid_cache_path("")
        , m_expired_cache_path("")
        , m_writable_pkgs_dir(caches.first_writable_path())
        , m_channel_id(channel.id())
        , m_platform(platform)
        , m_name(get_name(m_channel_id, m_platform))
        , m_repodata_fn(repodata_fn)
        , m_json_fn(cache_fn_url(name()))
        , m_solv_fn(m_json_fn.substr(0, m_json_fn.size() - 4) + "solv")
        , m_is_noarch(platform == "noarch")
        , p_context(&(ctx))
    {
        m_full_url = util::url_concat(channel.url().str(), "/", repodata_url_path());
        assert(!channel.is_package());
        m_forbid_cache = (channel.mirror_urls().size() == 1u)
                         && util::starts_with(channel.url().str(), "file://");
        load(caches, channel_context, channel);
    }

    std::string SubdirData::repodata_url_path() const
    {
        return util::concat(m_platform, "/", m_repodata_fn);
    }

    const std::string& SubdirData::repodata_full_url() const
    {
        return m_full_url;
    }

    void
    SubdirData::load(MultiPackageCache& caches, ChannelContext& channel_context, const specs::Channel& channel)
    {
        if (!m_forbid_cache)
        {
            load_cache(caches);
        }

        if (m_loaded)
        {
            Console::stream() << fmt::format("{:<50} {:>20}", name(), std::string("Using cache"));
        }
        else
        {
            LOG_INFO << "No valid cache found";
            if (!m_expired_cache_path.empty())
            {
                LOG_INFO << "Expired cache (or invalid mod/etag headers) found at '"
                         << m_expired_cache_path.string() << "'";
            }
            update_metadata_zst(channel_context, channel);
        }
    }

    void SubdirData::load_cache(MultiPackageCache& caches)
    {
        LOG_INFO << "Searching index cache file for repo '" << name() << "'";
        file_time_point now = fs::file_time_type::clock::now();

        const Context& context = *p_context;
        const auto cache_paths = without_duplicates(caches.paths());

        for (const fs::u8path& cache_path : cache_paths)
        {
            // TODO: rewrite this with pipe chains of ranges
            fs::u8path json_file = cache_path / "cache" / m_json_fn;
            if (!fs::is_regular_file(json_file))
            {
                continue;
            }

            auto lock = LockFile(cache_path / "cache");
            file_duration cache_age = get_cache_age(json_file, now);
            if (!is_valid(cache_age))
            {
                continue;
            }

            auto metadata_temp = SubdirMetadata::read(json_file);
            if (!metadata_temp.has_value())
            {
                LOG_INFO << "Invalid json cache found, ignoring";
                continue;
            }
            m_metadata = std::move(metadata_temp.value());

            const int max_age = get_max_age(
                m_metadata.cache_control(),
                static_cast<int>(context.local_repodata_ttl)
            );
            const auto cache_age_seconds = std::chrono::duration_cast<std::chrono::seconds>(cache_age)
                                               .count();

            if ((max_age > cache_age_seconds || context.offline || context.use_index_cache))
            {
                // valid json cache found
                if (!m_loaded)
                {
                    LOG_DEBUG << "Using JSON cache";
                    LOG_TRACE << "Cache age: " << cache_age_seconds << "/" << max_age << "s";

                    m_valid_cache_path = cache_path;
                    m_json_cache_valid = true;
                    m_loaded = true;
                }

                // check libsolv cache
                fs::u8path solv_file = cache_path / "cache" / m_solv_fn;
                file_duration solv_age = get_cache_age(solv_file, now);

                if (is_valid(solv_age) && solv_age <= cache_age)
                {
                    // valid libsolv cache found
                    LOG_DEBUG << "Using SOLV cache";
                    LOG_TRACE << "Cache age: "
                              << std::chrono::duration_cast<std::chrono::seconds>(solv_age).count()
                              << "s";
                    m_solv_cache_valid = true;
                    m_valid_cache_path = cache_path;
                    // no need to search for other valid caches
                    break;
                }
            }
            else
            {
                if (m_expired_cache_path.empty())
                {
                    m_expired_cache_path = cache_path;
                }
                LOG_DEBUG << "Expired cache or invalid mod/etag headers";
            }
        }
    }

    void
    SubdirData::update_metadata_zst(ChannelContext& channel_context, const specs::Channel& channel)
    {
        const Context& context = *p_context;
        if (!context.offline || m_forbid_cache)
        {
            m_metadata.set_zst(m_metadata.has_zst() || channel_context.has_zst(channel));
        }
    }

    download::MultiRequest SubdirData::build_check_requests()
    {
        download::MultiRequest request;

        if ((!p_context->offline || m_forbid_cache) && p_context->repodata_use_zst
            && !m_metadata.has_zst())
        {
            request.push_back(download::Request(
                name() + " (check zst)",
                download::MirrorName(m_channel_id),
                repodata_url_path() + ".zst",
                "",
                /* lhead_only = */ true,
                /* lignore_failure = */ true
            ));

            request.back().on_success = [this](const download::Success& success)
            {
                const std::string& effective_url = success.transfer.effective_url;
                int http_status = success.transfer.http_status;
                LOG_INFO << "Checked: " << effective_url << " [" << http_status << "]";
                if (util::ends_with(effective_url, ".zst"))
                {
                    m_metadata.set_zst(http_status == 200);
                }
                return expected_t<void>();
            };

            request.back().on_failure = [this](const download::Error& error)
            {
                if (error.transfer.has_value())
                {
                    LOG_INFO << "Checked: " << error.transfer.value().effective_url << " ["
                             << error.transfer.value().http_status << "]";
                }
                m_metadata.set_zst(false);
            };
        }
        return request;
    }

    download::Request SubdirData::build_index_request()
    {
        fs::u8path writable_cache_dir = create_cache_dir(m_writable_pkgs_dir);
        auto lock = LockFile(writable_cache_dir);
        m_temp_file = std::make_unique<TemporaryFile>("mambaf", "", writable_cache_dir);

        bool use_zst = m_metadata.has_zst();

        download::Request request(
            name(),
            download::MirrorName(m_channel_id),
            repodata_url_path() + (use_zst ? ".zst" : ""),
            m_temp_file->path().string(),
            /*head_only*/ false,
            /*ignore_failure*/ !m_is_noarch
        );
        request.etag = m_metadata.etag();
        request.last_modified = m_metadata.last_modified();

        request.on_success = [this](const download::Success& success)
        {
            if (success.transfer.http_status == 304)
            {
                return use_existing_cache();
            }
            else
            {
                return finalize_transfer(SubdirMetadata::HttpMetadata{ repodata_full_url(),
                                                                       success.etag,
                                                                       success.last_modified,
                                                                       success.cache_control });
            }
        };

        request.on_failure = [](const download::Error& error)
        {
            if (error.transfer.has_value())
            {
                LOG_WARNING << "Unable to retrieve repodata (response: "
                            << error.transfer.value().http_status << ") for '"
                            << error.transfer.value().effective_url << "'";
            }
            else
            {
                LOG_WARNING << error.message;
            }
            if (error.retry_wait_seconds.has_value())
            {
                LOG_WARNING << "Retrying in " << error.retry_wait_seconds.value() << " seconds";
            }
        };

        return request;
    }

    expected_t<void> SubdirData::use_existing_cache()
    {
        LOG_INFO << "Cache is still valid";

        fs::u8path json_file = m_expired_cache_path / "cache" / m_json_fn;
        fs::u8path solv_file = m_expired_cache_path / "cache" / m_solv_fn;

        if (path::is_writable(json_file)
            && (!fs::is_regular_file(solv_file) || path::is_writable(solv_file)))
        {
            LOG_DEBUG << "Refreshing cache files ages";
            m_valid_cache_path = m_expired_cache_path;
        }
        else
        {
            if (m_writable_pkgs_dir.empty())
            {
                LOG_ERROR << "Could not find any writable cache directory for repodata file";
                return make_unexpected(
                    "Could not find any writable cache directory for repodata file",
                    mamba_error_code::subdirdata_not_loaded
                );
            }

            LOG_DEBUG << "Copying repodata cache files from '" << m_expired_cache_path.string()
                      << "' to '" << m_writable_pkgs_dir.string() << "'";
            fs::u8path writable_cache_dir = get_cache_dir(m_writable_pkgs_dir);
            auto lock = LockFile(writable_cache_dir);

            fs::u8path copied_json_file = writable_cache_dir / m_json_fn;
            json_file = replace_file(copied_json_file, json_file);

            if (fs::is_regular_file(solv_file))
            {
                auto copied_solv_file = writable_cache_dir / m_solv_fn;
                solv_file = replace_file(copied_solv_file, solv_file);
            }

            m_valid_cache_path = m_writable_pkgs_dir;
        }

        refresh_last_write_time(json_file, solv_file);

        m_temp_file.reset();
        m_loaded = true;
        return expected_t<void>();
    }

    expected_t<void> SubdirData::finalize_transfer(SubdirMetadata::HttpMetadata http_data)
    {
        if (m_writable_pkgs_dir.empty())
        {
            LOG_ERROR << "Could not find any writable cache directory for repodata file";
            return make_unexpected(
                "Could not find any writable cache directory for repodata file",
                mamba_error_code::subdirdata_not_loaded
            );
        }

        LOG_DEBUG << "Finalized transfer of '" << http_data.url << "'";

        m_metadata.store_http_metadata(std::move(http_data));

        fs::u8path writable_cache_dir = get_cache_dir(m_writable_pkgs_dir);
        fs::u8path json_file = writable_cache_dir / m_json_fn;
        auto lock = LockFile(writable_cache_dir);

        fs::u8path state_file = json_file;
        state_file.replace_extension(".state.json");
        std::error_code ec;
        mamba_fs::rename_or_move(m_temp_file->path(), json_file, ec);
        if (ec)
        {
            std::string error = fmt::format(
                "Could not move repodata file from {} to {}: {}",
                m_temp_file->path(),
                json_file,
                strerror(errno)
            );
            LOG_ERROR << error;
            return make_unexpected(error, mamba_error_code::subdirdata_not_loaded);
        }

        m_metadata.store_file_metadata(json_file);
        m_metadata.write(state_file);

        m_temp_file.reset();
        m_valid_cache_path = m_writable_pkgs_dir;
        m_json_cache_valid = true;
        m_loaded = true;

        return expected_t<void>();
    }

    void SubdirData::refresh_last_write_time(const fs::u8path& json_file, const fs::u8path& solv_file)
    {
        const auto now = fs::file_time_type::clock::now();

        file_duration json_age = get_cache_age(json_file, now);
        file_duration solv_age = get_cache_age(solv_file, now);

        {
            auto lock = LockFile(json_file);
            fs::last_write_time(json_file, fs::now());
            m_json_cache_valid = true;
        }

        if (fs::is_regular_file(solv_file) && solv_age.count() <= json_age.count())
        {
            auto lock = LockFile(solv_file);
            fs::last_write_time(solv_file, fs::now());
            m_solv_cache_valid = true;
        }

        fs::u8path state_file = json_file;
        state_file.replace_extension(".state.json");
        auto lock = LockFile(state_file);
        m_metadata.store_file_metadata(json_file);
        m_metadata.write(state_file);
    }

    std::string cache_name_from_url(std::string_view url)
    {
        auto u = std::string(url);
        if (u.empty() || (u.back() != '/' && !util::ends_with(u, ".json")))
        {
            u += '/';
        }

        // mimicking conda's behavior by special handling repodata.json
        // todo support .zst
        if (util::ends_with(u, "/repodata.json"))
        {
            u = u.substr(0, u.size() - 13);
        }
        return util::Md5Hasher().str_hex_str(u).substr(0, 8u);
    }

    std::string cache_fn_url(const std::string& url)
    {
        return cache_name_from_url(url) + ".json";
    }

    std::string create_cache_dir(const fs::u8path& cache_path)
    {
        const auto cache_dir = cache_path / "cache";
        fs::create_directories(cache_dir);

        // Some filesystems don't support special permissions such as setgid on directories (e.g.
        // NFS). and fail if we try to set the setgid bit on the cache directory.
        //
        // We want to set the setgid bit on the cache directory to preserve the permissions as much
        // as possible if we can; hence we proceed in two steps to set the permissions by
        //   1. Setting the permissions without the setgid bit to the desired value without.
        //   2. Trying to set the setgid bit on the directory and report success or failure in log
        //   without raising an error or propagating an error which was raised.

        const auto permissions = fs::perms::owner_all | fs::perms::group_all
                                 | fs::perms::others_read | fs::perms::others_exec;
        fs::permissions(cache_dir, permissions, fs::perm_options::replace);
        LOG_TRACE << "Set permissions on cache directory " << cache_dir << " to 'rwxrwxr-x'";

        std::error_code ec;
        fs::permissions(cache_dir, fs::perms::set_gid, fs::perm_options::add, ec);

        if (!ec)
        {
            LOG_TRACE << "Set setgid bit on cache directory " << cache_dir;
        }
        else
        {
            LOG_TRACE << "Could not set setgid bit on cache directory " << cache_dir
                      << "\nReason:" << ec.message() << "; ignoring and continuing";
        }

        return cache_dir.string();
    }
}  // namespace mamba
