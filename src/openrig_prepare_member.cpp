#include "openrig_member.h"

#include <logonomic_cli_addon/json.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs    = std::filesystem;

class File_descriptor
{
public:
    explicit File_descriptor(int value) : m_value(value) {}
    ~File_descriptor()
    {
        if (m_value >= 0) {
            ::close(m_value);
        }
    }
    File_descriptor(const File_descriptor&) = delete;
    File_descriptor& operator=(const File_descriptor&) = delete;
    int get() const { return m_value; }

private:
    int m_value;
};

class Temporary_file
{
public:
    explicit Temporary_file(const fs::path& directory)
        : m_path((directory / ".openrig-member-XXXXXX").string()),
          m_file(::mkstemp(m_path.data()))
    {}

    ~Temporary_file()
    {
        if (m_file.get() >= 0 && !m_path.empty()) {
            ::unlink(m_path.c_str());
        }
    }
    int descriptor() const { return m_file.get(); }
    const char* path() const { return m_path.c_str(); }
    bool remove()
    {
        if (::unlink(m_path.c_str()) != 0) {
            return false;
        }
        m_path.clear();
        return true;
    }

private:
    std::string m_path;
    File_descriptor m_file;
};

struct Preparation_arguments
{
    fs::path input;
    fs::path output;
    openrig::Authored_member_selector selector;
    std::vector<std::string> prefix;
};

int refuse(int exit_code, std::string_view message)
{
    std::cerr << "openrig_prepare_member: " << message << '\n';
    return exit_code;
}

std::optional<Preparation_arguments> parse_arguments(int argc, char* argv[])
{
    constexpr std::array<std::string_view, 7> names = {
        "--spec", "--output", "--rig", "--member", "--expect-wrapper", "--wrapper-prefix", "--pod",
    };
    std::array<std::optional<std::string_view>, names.size()> values;
    for (int index = 1; index < argc; index += 2) {
        const auto name = std::find(names.begin(), names.end(), argv[index]);
        if (name == names.end() || index + 1 >= argc) {
            refuse(2, "Expected a named option and its value; use --help for the command syntax.");
            return std::nullopt;
        }
        auto& value = values[std::size_t(name - names.begin())];
        if (value || !*argv[index + 1]) {
            refuse(2, "Every option must have one nonempty value and occur only once.");
            return std::nullopt;
        }
        value = argv[index + 1];
    }
    for (std::size_t index = 0; index < names.size() - 1; ++index) {
        if (!values[index]) {
            refuse(2, std::string("Missing required option ") + std::string(names[index]) + '.');
            return std::nullopt;
        }
    }

    auto expected = addon::parse_json(*values[4], {}, nullptr);
    auto prefix   = addon::parse_json(*values[5], {}, nullptr);
    const auto string_array = [](const addon::Json_value& value) {
        return value.is_array() &&
            std::all_of(value.as_array().begin(), value.as_array().end(),
                [](const addon::Json_value& item) { return item.is_string(); });
    };
    if (!expected || (!expected->is_null() && !string_array(*expected)) ||
        !prefix || !string_array(*prefix))
    {
        refuse(2, "Expected wrapper must be null or a JSON string array; prefix must be a JSON string array.");
        return std::nullopt;
    }

    Preparation_arguments arguments;
    arguments.input  = *values[0];
    arguments.output = *values[1];
    arguments.selector.rig_name         = *values[2];
    arguments.selector.member_id        = *values[3];
    arguments.selector.expected_wrapper = std::move(expected);
    if (values[6]) {
        arguments.selector.pod_id = std::string(*values[6]);
    }
    for (const auto& argument : prefix->as_array()) {
        arguments.prefix.push_back(argument.as_string());
    }
    return arguments;
}

std::optional<std::string> read_specification(const fs::path& path)
{
    const File_descriptor input(::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    if (input.get() < 0) {
        refuse(1, std::string("Cannot open the source specification: ") + std::strerror(errno));
        return std::nullopt;
    }
    struct stat status {};
    if (::fstat(input.get(), &status) != 0) {
        refuse(1, std::string("Cannot inspect the source specification: ") + std::strerror(errno));
        return std::nullopt;
    }
    if (!S_ISREG(status.st_mode)) {
        refuse(1, "The source specification must be a regular file.");
        return std::nullopt;
    }

    std::string bytes;
    std::array<char, 8192> buffer;
    const auto max_bytes = addon::Json_parse_limits{}.max_bytes;
    while (true) {
        const ssize_t count = ::read(input.get(), buffer.data(), buffer.size());
        if (count == 0) {
            return bytes;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            refuse(1, std::string("Cannot read the source specification: ") + std::strerror(errno));
            return std::nullopt;
        }
        if (bytes.size() + std::uint64_t(count) > max_bytes) {
            refuse(1, "The source specification exceeds the supported JSON document size.");
            return std::nullopt;
        }
        bytes.append(buffer.data(), std::size_t(count));
    }
}

bool publish_new_file(const fs::path& output, std::string_view bytes)
{
    // link() publishes the fully written inode only if the destination is
    // absent. Unlike rename(), it cannot silently replace a concurrent edit.
    Temporary_file temporary(output.parent_path());
    if (temporary.descriptor() < 0) {
        refuse(1, std::string("Cannot create the temporary specification: ") + std::strerror(errno));
        return false;
    }
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t count = ::write(
            temporary.descriptor(), bytes.data() + written, bytes.size() - written);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            const int error = count < 0 ? errno : EIO;
            refuse(1, std::string("Cannot write the prepared specification: ") + std::strerror(error));
            return false;
        }
        written += std::size_t(count);
    }
    if (::fsync(temporary.descriptor()) != 0) {
        const int error = errno;
        refuse(1, std::string("Cannot flush the prepared specification: ") + std::strerror(error));
        return false;
    }
    if (::link(temporary.path(), output.c_str()) != 0) {
        const int error = errno;
        refuse(1, error == EEXIST
            ? "The output path already exists; nothing was replaced."
            : std::string("Cannot publish the prepared specification: ") + std::strerror(error));
        return false;
    }
    if (!temporary.remove()) {
        refuse(1, "The output was created, but its temporary file could not be removed.");
        return false;
    }
    return true;
}

int prepare(const Preparation_arguments& arguments)
{
    std::error_code error;
    const fs::path input = fs::absolute(arguments.input, error).lexically_normal();
    if (error) {
        return refuse(1, "Cannot resolve the source specification path.");
    }
    const fs::path output = fs::absolute(arguments.output, error).lexically_normal();
    if (error || output.filename().empty() || output.filename() == "." || output.filename() == "..") {
        return refuse(2, "The output must name a new file beside the source specification.");
    }
    // OpenRig roots relative spec paths at dirname(path.resolve(sourceRef)).
    // Resolving directory symlinks here would change the meaning of ../ paths.
    const fs::path source_directory = input.parent_path();
    const fs::path output_directory = output.parent_path();
    if (source_directory != output_directory) {
        return refuse(2, "Output must stay beside the source specification to preserve relative paths.");
    }

    auto source_bytes = read_specification(input);
    if (!source_bytes) {
        return 1;
    }
    auto specification = addon::parse_json(*source_bytes, {}, nullptr);
    if (!specification) {
        return refuse(2, "The source specification must be a supported JSON document.");
    }
    std::string preparation_error;
    auto prepared = openrig::prepare_authored_member(
        *specification, arguments.selector, arguments.prefix, &preparation_error);
    if (!prepared) {
        return refuse(2, preparation_error);
    }
    const std::string output_bytes = prepared->changed
        ? addon::serialize_json(prepared->specification) + '\n'
        : std::move(*source_bytes);
    return publish_new_file(output_directory / output.filename(), output_bytes) ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout <<
                "Prepare one member in a JSON-authored OpenRig specification.\n"
                "Usage: openrig_prepare_member --spec INPUT --output OUTPUT --rig NAME\n"
                "       [--pod ID] --member ID --expect-wrapper JSON --wrapper-prefix JSON\n"
                "OUTPUT must be a new file in INPUT's directory; INPUT is unchanged.\n"
                "Expected wrapper: null for absence, or the exact existing argv array.\n"
                "Prefix: a JSON argv array naming a caller-supplied stable, nonsecret\n"
                "wrapper, with exactly one terminal -- argument after its executable.\n"
                "This prepares a spec; it does not install a wrapper or enroll a live seat.\n";
            return 0;
        }
        auto arguments = parse_arguments(argc, argv);
        return arguments ? prepare(*arguments) : 2;
    }
    catch (const std::exception&) {
        return refuse(1, "Specification preparation could not complete.");
    }
}
