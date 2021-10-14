// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <bitset>
#include "inc/Test.h"
#include "inc/Helper/VectorSetReader.h"
#include "inc/Core/Common/PQQuantizer.h"
#include <random>
#include "inc/Helper/SimpleIniReader.h"
#include "inc/Core/VectorIndex.h"
#include "inc/Core/Common/CommonUtils.h"

#include <unordered_set>
#include <ctime>
#include <inc/Core/Common.h>




template <typename T>
void Build(SPTAG::IndexAlgoType algo, std::string distCalcMethod, std::shared_ptr<SPTAG::VectorSet>& vec, std::shared_ptr<SPTAG::MetadataSet>& meta, const std::string out)
{

    std::shared_ptr<SPTAG::VectorIndex> vecIndex = SPTAG::VectorIndex::CreateInstance(algo, SPTAG::GetEnumValueType<T>());
    BOOST_CHECK(nullptr != vecIndex);

    vecIndex->SetParameter("DistCalcMethod", distCalcMethod);
    vecIndex->SetParameter("NumberOfThreads", "8");

    BOOST_CHECK(SPTAG::ErrorCode::Success == vecIndex->BuildIndex(vec, meta));
    BOOST_CHECK(SPTAG::ErrorCode::Success == vecIndex->SaveIndex(out));
}

template <typename T>
void BuildWithMetaMapping(SPTAG::IndexAlgoType algo, std::string distCalcMethod, std::shared_ptr<SPTAG::VectorSet>& vec, std::shared_ptr<SPTAG::MetadataSet>& meta, const std::string out)
{

    std::shared_ptr<SPTAG::VectorIndex> vecIndex = SPTAG::VectorIndex::CreateInstance(algo, SPTAG::GetEnumValueType<T>());
    BOOST_CHECK(nullptr != vecIndex);

    vecIndex->SetParameter("DistCalcMethod", distCalcMethod);
    vecIndex->SetParameter("NumberOfThreads", "8");

    BOOST_CHECK(SPTAG::ErrorCode::Success == vecIndex->BuildIndex(vec, meta, true));
    BOOST_CHECK(SPTAG::ErrorCode::Success == vecIndex->SaveIndex(out));
}

template <typename T>
void Search(const std::string folder, T* vec, SPTAG::SizeType n, int k, std::string* truthmeta)
{
    std::shared_ptr<SPTAG::VectorIndex> vecIndex;
    BOOST_CHECK(SPTAG::ErrorCode::Success == SPTAG::VectorIndex::LoadIndex(folder, vecIndex));
    BOOST_CHECK(nullptr != vecIndex);

    for (SPTAG::SizeType i = 0; i < n; i++)
    {
        SPTAG::QueryResult res(vec, k, true);
        vecIndex->SearchIndex(res);
        std::unordered_set<std::string> resmeta;
        for (int j = 0; j < k; j++)
        {
            resmeta.insert(std::string((char*)res.GetMetadata(j).Data(), res.GetMetadata(j).Length()));
            std::cout << res.GetResult(j)->Dist << "@(" << res.GetResult(j)->VID << "," << std::string((char*)res.GetMetadata(j).Data(), res.GetMetadata(j).Length()) << ") ";
        }
        std::cout << std::endl;
        for (int j = 0; j < k; j++)
        {
            //BOOST_CHECK(resmeta.count(truthmeta[i * k + j]));
        }
        vec += vecIndex->GetFeatureDim();
    }
    vecIndex.reset();
}

template <typename T>
void Add(const std::string folder, std::shared_ptr<SPTAG::VectorSet>& vec, std::shared_ptr<SPTAG::MetadataSet>& meta, const std::string out)
{
    std::shared_ptr<SPTAG::VectorIndex> vecIndex;
    BOOST_CHECK(SPTAG::ErrorCode::Success == SPTAG::VectorIndex::LoadIndex(folder, vecIndex));
    BOOST_CHECK(nullptr != vecIndex);

    BOOST_CHECK(SPTAG::ErrorCode::Success == vecIndex->AddIndex(vec, meta));
    BOOST_CHECK(SPTAG::ErrorCode::Success == vecIndex->SaveIndex(out));
    vecIndex.reset();
}

template <typename T>
void AddOneByOne(SPTAG::IndexAlgoType algo, std::string distCalcMethod, std::shared_ptr<SPTAG::VectorSet>& vec, std::shared_ptr<SPTAG::MetadataSet>& meta, const std::string out)
{
    std::shared_ptr<SPTAG::VectorIndex> vecIndex = SPTAG::VectorIndex::CreateInstance(algo, SPTAG::GetEnumValueType<T>());
    BOOST_CHECK(nullptr != vecIndex);

    vecIndex->SetParameter("DistCalcMethod", distCalcMethod);
    vecIndex->SetParameter("NumberOfThreads", "8");

    clock_t start = clock();
    for (SPTAG::SizeType i = 0; i < vec->Count(); i++) {
        SPTAG::ByteArray metaarr = meta->GetMetadata(i);
        std::uint64_t offset[2] = { 0, metaarr.Length() };
        std::shared_ptr<SPTAG::MetadataSet> metaset(new SPTAG::MemMetadataSet(metaarr, SPTAG::ByteArray((std::uint8_t*)offset, 2 * sizeof(std::uint64_t), false), 1));
        SPTAG::ErrorCode ret = vecIndex->AddIndex(vec->GetVector(i), 1, vec->Dimension(), metaset);
        if (SPTAG::ErrorCode::Success != ret) std::cerr << "Error AddIndex(" << (int)(ret) << ") for vector " << i << std::endl;
    }
    std::cout << "AddIndex time: " << ((float)(clock() - start) / CLOCKS_PER_SEC / vec->Count()) << "s" << std::endl;

    Sleep(10000);

    BOOST_CHECK(SPTAG::ErrorCode::Success == vecIndex->SaveIndex(out));
}

template <typename T>
void Delete(const std::string folder, T* vec, SPTAG::SizeType n, const std::string out)
{
    std::shared_ptr<SPTAG::VectorIndex> vecIndex;
    BOOST_CHECK(SPTAG::ErrorCode::Success == SPTAG::VectorIndex::LoadIndex(folder, vecIndex));
    BOOST_CHECK(nullptr != vecIndex);

    BOOST_CHECK(SPTAG::ErrorCode::Success == vecIndex->DeleteIndex((const void*)vec, n));
    BOOST_CHECK(SPTAG::ErrorCode::Success == vecIndex->SaveIndex(out));
    vecIndex.reset();
}

template <typename T>
void Test(SPTAG::IndexAlgoType algo, std::string distCalcMethod, SPTAG::SizeType n, SPTAG::DimensionType m)
{
    SPTAG::SizeType q = 1000;
    int k = 3;
    std::vector<T> vec;
    for (SPTAG::SizeType i = 0; i < n; i++) {
        for (SPTAG::DimensionType j = 0; j < m; j++) {
            vec.push_back((T)i);
        }
    }

    std::vector<T> query;
    for (SPTAG::SizeType i = 0; i < q; i++) {
        for (SPTAG::DimensionType j = 0; j < m; j++) {
            query.push_back((T)i * 2);
        }
    }

    std::vector<char> meta;
    std::vector<std::uint64_t> metaoffset;
    for (SPTAG::SizeType i = 0; i < n; i++) {
        metaoffset.push_back((std::uint64_t)meta.size());
        std::string a = std::to_string(i);
        for (size_t j = 0; j < a.length(); j++)
            meta.push_back(a[j]);
    }
    metaoffset.push_back((std::uint64_t)meta.size());

    std::shared_ptr<SPTAG::VectorSet> vecset(new SPTAG::BasicVectorSet(
        SPTAG::ByteArray((std::uint8_t*)vec.data(), sizeof(T) * n * m, false),
        SPTAG::GetEnumValueType<T>(), m, n));

    std::shared_ptr<SPTAG::MetadataSet> metaset(new SPTAG::MemMetadataSet(
        SPTAG::ByteArray((std::uint8_t*)meta.data(), meta.size() * sizeof(char), false),
        SPTAG::ByteArray((std::uint8_t*)metaoffset.data(), metaoffset.size() * sizeof(std::uint64_t), false),
        n));

    Build<T>(algo, distCalcMethod, vecset, metaset, "testindices-pq");
    std::string truthmeta1[] = { "0", "1", "2", "2", "1", "3", "4", "3", "5", "4", "3", "5" };
    Search<T>("testindices-pq", query.data(), q, k, truthmeta1);
    /*
    Add<T>("testindices-pq", vecset, metaset, "testindices-pq");
    std::string truthmeta2[] = { "0", "0", "1", "2", "2", "1", "4", "4", "3" };
    Search<T>("testindices-pq", query.data(), q, k, truthmeta2);

    Delete<T>("testindices-pq", query.data(), q, "testindices-pq");
    std::string truthmeta3[] = { "1", "1", "3", "1", "3", "1", "3", "5", "3" };
    Search<T>("testindices-pq", query.data(), q, k, truthmeta3);

    BuildWithMetaMapping<T>(algo, distCalcMethod, vecset, metaset, "testindices-pq");
    std::string truthmeta4[] = { "0", "1", "2", "2", "1", "3", "4", "3", "5" };
    Search<T>("testindices-pq", query.data(), q, k, truthmeta4);

    Add<T>("testindices-pq", vecset, metaset, "testindices-pq");
    std::string truthmeta5[] = { "0", "1", "2", "2", "1", "3", "4", "3", "5" };
    Search<T>("testindices-pq", query.data(), q, k, truthmeta5);

    AddOneByOne<T>(algo, distCalcMethod, vecset, metaset, "testindices-pq");
    std::string truthmeta6[] = { "0", "1", "2", "2", "1", "3", "4", "3", "5" };
    Search<T>("testindices-pq", query.data(), q, k, truthmeta6);
    */
}

void TestPQDistance(float minVecVal, float maxVecVal, int numVecs, int vectorDim, int M) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dist(minVecVal, maxVecVal);
    int Ks = 256;

    std::string CODEBOOK_FILE = "test-quantizer.bin";
    float* codebooks = new float[M * Ks * (vectorDim / M)];
    for (int i = 0; i < M; i++) {
        float* base = codebooks + i * Ks * (vectorDim / M);
        for (int j = 0; j < Ks; j++) {
            for (int k = 0; k < (vectorDim / M); k++) {
                *(base + j * (vectorDim / M) + k) = dist(gen);
            }
        }
    }

    auto baseQuantizer = std::make_shared<SPTAG::COMMON::PQQuantizer>(M, Ks, (vectorDim / M), codebooks);
    auto ptr = SPTAG::f_createIO();
    BOOST_ASSERT(ptr != nullptr && ptr->Initialize(CODEBOOK_FILE.c_str(), std::ios::binary | std::ios::out));
    baseQuantizer->SaveQuantizer(ptr);
    ptr->ShutDown();

    BOOST_ASSERT(ptr->Initialize(CODEBOOK_FILE.c_str(), std::ios::binary | std::ios::in));
    SPTAG::COMMON::Quantizer::LoadQuantizer(ptr, SPTAG::QuantizerType::PQQuantizer);
    auto loadedQuantizer = SPTAG::COMMON::DistanceUtils::Quantizer;
    BOOST_ASSERT(loadedQuantizer != nullptr);

    float* vecs = new float[numVecs * vectorDim];
    for (int i = 0; i < numVecs * vectorDim; i++) {
        vecs[i] = dist(gen);
    }

    std::vector<std::uint8_t> baseQ(loadedQuantizer->GetNumSubvectors()), loadQ(loadedQuantizer->GetNumSubvectors()),
        baseQ2(loadedQuantizer->GetNumSubvectors()), loadQ2(loadedQuantizer->GetNumSubvectors());
    for (int i = 0; i < numVecs; i++) {
        auto vec = &vecs[i * vectorDim];

        baseQuantizer->QuantizeVector(vec, baseQ.data());
        loadedQuantizer->QuantizeVector(vec, loadQ.data());
        for (int j = 0; j < M; j++) {
            BOOST_ASSERT(baseQ[j] == loadQ[j]);
        }
        for (int j = i; j < numVecs; j++) {
            auto vec2 = &vecs[j * vectorDim];

            baseQuantizer->QuantizeVector(vec2, baseQ2.data());
            loadedQuantizer->QuantizeVector(vec2, loadQ2.data());
            //BOOST_CHECK_CLOSE_FRACTION(baseQuantizer->CosineDistance(baseQ.data(), baseQ2.data()), loadedQuantizer->CosineDistance(baseQ.data(), baseQ2.data()), 1e-4);
            //BOOST_CHECK_CLOSE_FRACTION(baseQuantizer->L2Distance(baseQ.data(), baseQ2.data()), loadedQuantizer->L2Distance(baseQ.data(), baseQ2.data()), 1e-4);
            //BOOST_CHECK_CLOSE_FRACTION(SPTAG::COMMON::DistanceUtils::ComputeDistance<float>(vec, vec2, vectorDim, SPTAG::DistCalcMethod::Cosine), baseQuantizer->CosineDistance(baseQ.data(), baseQ2.data()), 5e-1);
            //BOOST_CHECK_CLOSE_FRACTION(SPTAG::COMMON::DistanceUtils::ComputeDistance<float>(vec, vec2, vectorDim, SPTAG::DistCalcMethod::L2), baseQuantizer->L2Distance(baseQ.data(), baseQ2.data()), 5e-1);
        }
    }
    delete[] vecs;
    SPTAG::COMMON::DistanceUtils::Quantizer = nullptr;
}

void GenerateQuantizer(int m) {
    int M = 5;
    int Ks = 256;

    std::string CODEBOOK_FILE = "test-quantizer-tree.bin";
    float* codebooks = new float[M * Ks * (m / M)];
    for (int i = 0; i < M; i++) {
        float* base = codebooks + i * Ks * (m / M);
        for (int j = 0; j < Ks; j++) {
            for (int k = 0; k < (m / M); k++) {
                *(base + j * (m / M) + k) = (float)j;
            }
        }
    }
    auto baseQuantizer = std::make_shared<SPTAG::COMMON::PQQuantizer>(M, Ks, (m / M), codebooks);
    auto ptr = SPTAG::f_createIO();
    BOOST_ASSERT(ptr != nullptr && ptr->Initialize(CODEBOOK_FILE.c_str(), std::ios::binary | std::ios::out));
    baseQuantizer->SaveQuantizer(ptr);
    ptr->ShutDown();

    BOOST_ASSERT(ptr->Initialize(CODEBOOK_FILE.c_str(), std::ios::binary | std::ios::in));
    SPTAG::COMMON::Quantizer::LoadQuantizer(ptr, SPTAG::QuantizerType::PQQuantizer);
    BOOST_ASSERT(SPTAG::COMMON::DistanceUtils::Quantizer != nullptr);
}

BOOST_AUTO_TEST_SUITE(QuantizationTest)

BOOST_AUTO_TEST_CASE(PQDistanceTest)
{
    TestPQDistance(0.1f, 1.0f, 5, 10, 5);
}


BOOST_AUTO_TEST_CASE(KDTTest)
{
    int n = 3000;
    int m = 400;

    GenerateQuantizer(m);

    Test<std::uint8_t>(SPTAG::IndexAlgoType::KDT, "L2", n, m);

    SPTAG::COMMON::DistanceUtils::Quantizer = nullptr;
}

BOOST_AUTO_TEST_CASE(BKTTest)
{
    int n = 200;
    int m = 20;

    GenerateQuantizer(m);

    Test<std::uint8_t>(SPTAG::IndexAlgoType::BKT, "L2", n, m);

    SPTAG::COMMON::DistanceUtils::Quantizer = nullptr;
}

BOOST_AUTO_TEST_SUITE_END()