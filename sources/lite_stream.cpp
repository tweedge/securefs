#include "lite_stream.h"

namespace securefs
{
std::string LiteCorruptedStreamException::message() const { return "Stream is corrupted"; }
const char* LiteCorruptedStreamException::type_name() const noexcept
{
    return "LiteCorruptedStreamException";
}

LiteAESGCMCryptStream::LiteAESGCMCryptStream(std::shared_ptr<StreamBase> stream,
                                             const key_type& master_key,
                                             const id_type& id,
                                             unsigned int block_size,
                                             unsigned iv_size,
                                             bool check)
    : BlockBasedStream(block_size)
    , m_stream(std::move(stream))
    , m_id(id)
    , m_iv_size(iv_size)
    , m_check(check)
{
    if (m_iv_size < 12 || m_iv_size > 32)
        throwInvalidArgumentException("IV size too small or too large");
    if (!m_stream)
        throwInvalidArgumentException("Null stream");
    if (block_size < 32)
        throwInvalidArgumentException("Block size too small");

    key_type header;
    auto rc = m_stream->read(header.data(), 0, header.size());
    if (rc == 0)
    {
        generate_random(m_session_key.data(), m_session_key.size());
        byte_xor(m_session_key.data(), master_key.data(), header.data(), header.size());
        m_stream->write(header.data(), 0, header.size());
    }
    else if (rc == header.size())
    {
        byte_xor(header.data(), master_key.data(), m_session_key.data(), m_session_key.size());
    }
    else
    {
        throwInvalidArgumentException("Underlying stream has invalid header size");
    }

    m_buffer.reset(new byte[get_underlying_block_size()]);
    m_aux_buffer.reset(new byte[get_auxiliary_buffer_size()]);
}

LiteAESGCMCryptStream::~LiteAESGCMCryptStream() {}

void LiteAESGCMCryptStream::flush() { m_stream->flush(); }

bool LiteAESGCMCryptStream::is_sparse() const noexcept { return m_stream->is_sparse(); }

length_type LiteAESGCMCryptStream::read_block(offset_type block_number, void* output)
{
    length_type rc = m_stream->read(m_buffer.get(),
                                    get_header_size() + get_underlying_block_size() * block_number,
                                    get_underlying_block_size());
    if (rc <= get_mac_size() + get_iv_size())
        return 0;

    if (rc > get_underlying_block_size())
        throwInvalidArgumentException("Invalid read");

    auto out_size = rc - get_iv_size() - get_mac_size();

    if (is_all_zeros(m_buffer.get(), rc))
    {
        memset(output, 0, get_block_size());
        return out_size;
    }

    memcpy(m_aux_buffer.get(), m_id.data(), m_id.size());
    to_little_endian<std::uint64_t>(block_number, m_aux_buffer.get() + m_id.size());

    bool success = aes_gcm_decrypt(m_buffer.get() + get_iv_size(),
                                   out_size,
                                   m_aux_buffer.get(),
                                   get_auxiliary_buffer_size(),
                                   m_session_key.data(),
                                   m_session_key.size(),
                                   m_buffer.get(),
                                   get_iv_size(),
                                   m_buffer.get() + rc - get_mac_size(),
                                   get_mac_size(),
                                   output);
    if (m_check && !success)
        throw MessageVerificationException(m_id, block_number * get_block_size());

    return out_size;
}

void LiteAESGCMCryptStream::write_block(offset_type block_number,
                                        const void* input,
                                        length_type size)
{
    memcpy(m_aux_buffer.get(), m_id.data(), m_id.size());
    to_little_endian<std::uint64_t>(block_number, m_aux_buffer.get() + m_id.size());

    do
    {
        generate_random(m_buffer.get(), get_iv_size());
    } while (is_all_zeros(m_buffer.get(), get_iv_size()));

    aes_gcm_encrypt(input,
                    size,
                    m_aux_buffer.get(),
                    get_auxiliary_buffer_size(),
                    m_session_key.data(),
                    m_session_key.size(),
                    m_buffer.get(),
                    get_iv_size(),
                    m_buffer.get() + get_iv_size() + size,
                    get_mac_size(),
                    m_buffer.get() + get_iv_size());

    m_stream->write(m_buffer.get(),
                    block_number * get_underlying_block_size() + get_header_size(),
                    size + get_iv_size() + get_mac_size());
}

length_type LiteAESGCMCryptStream::size() const
{
    auto underlying_size = m_stream->size();
    if (underlying_size <= get_header_size())
        return 0;
    underlying_size -= get_header_size();
    auto num_blocks = underlying_size / get_underlying_block_size();
    auto residue = underlying_size % get_underlying_block_size();
    return num_blocks * get_block_size() + (residue > (get_iv_size() + get_mac_size())
                                                ? residue - get_iv_size() - get_mac_size()
                                                : 0);
}

void LiteAESGCMCryptStream::adjust_logical_size(length_type length)
{
    auto new_blocks = length / get_block_size();
    auto residue = length % get_block_size();
    m_stream->resize(get_header_size() + new_blocks * get_underlying_block_size() + residue > 0
                         ? residue + get_iv_size() + get_mac_size()
                         : 0);
}
}