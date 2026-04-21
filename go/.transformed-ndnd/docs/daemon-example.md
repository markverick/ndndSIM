# NDNd Daemon Usage Example

These examples demonstrate how to use NDNd to create a small NDN network.

NDNd is the combined daemon with YaNFD (an NDN Forwarder) and ndn-dv (an NDN Router). While the example below uses the combined daemon, you can also use YaNFD and ndn-dv separately (or, for example, [NFD](https://github.com/named-data/NFD) with ndn-dv). The configuration in this case is identical, with the exception of running the two instances separately.

As a prerequisite, you must have NDNd installed on all nodes in the network. See the project [README](https://github.com/named-data/ndnd) for instructions on how to use the prebuilt binaries or build from source.

## File Transfer Example

This example demonstrates a 2-node network where one node (`bob`) fetches a file from the other node (`alice`).

On both nodes, we use the simple configuration file below. Put this config in a `conf.yml` file on each node. Remember to replace `<node-name>` with the actual node name (in this case, `alice` and `bob`).

Full configuration examples with documentation for routing (`dv`) and forwarding (`fw`) can be found [here](../dv/dv.sample.yml) and [here](../fw/yanfd.sample.yml) respectively.

```yaml
dv:
  network: /testnet
  router: /testnet/<node-name>
  keychain: "insecure"

fw:
  faces:
    udp:
      enabled_unicast: true
      enabled_multicast: false
      port_unicast: 6363
    tcp:
      enabled: true
      port_unicast: 6363
    websocket:
      enabled: false
  fw:
    threads: 2
```

1. The `network` name must be the same on all nodes in the network.
2. The `router` name must be unique for each node in the network.
3. The `faces` section configures the faces that the forwarder will listen on. In this case, we enable UDP and TCP faces on port 6363. The unix face is enabled by default and listens on `/run/nfd/nfd/sock`.
4. For this simple example, we disable routing security with `insecure`, which is not recommended for production use.

Once the configuration file is in place, start the NDNd daemon on each node:

```sh
# root permissions are requird to bind to the default unix socket
sudo ndnd daemon conf.yml
```

Once the daemons are running, we create a routing neighbor relationship between the two nodes. On `alice`, run:

```sh
# if udp is blocked, use tcp instead
ndnd dv link-create "udp://<bob-ip>:6363"

# replace <bob-ip> with the IP address of the bob node, e.g.
# ndnd dv link-create "udp://192.168.1.5:6363"
```

After a few seconds, logs should show up on both nodes indicating that the neighbor relationship has been established.
You can make this relationship permanent by adding the link to the configuration file:

```yaml
dv:
  ...
  neighbors:
    - uri: "udp://<bob-ip>:6363"  # e.g., "udp://192.168.1.5:6363"
```

Now, we can start serving the file on `alice` using the `put` tool:

```sh
# create a file to serve
echo "Hello, NDN!" > /tmp/hello.txt

# serve the file using put
# -expose enables advertising the file to the routing protocol
ndnd put -expose /alice/hello < /tmp/hello.txt
```

Logs on both sides should show the newly announced prefix `/alice/hello`.
On `bob`, we can now fetch the file using the `cat` tool:

```sh
# fetch the file using cat
ndnd cat /alice/hello > /tmp/hello.txt

# check the contents of the file
cat /tmp/hello.txt
```

This concludes the file transfer example. You can now experiment with more complex topologies by adding more nodes and creating more routing relationships. The routing protocol will automatically propagate prefixes to all nodes in the network.

## Routing Security Example

This example builds on the previous one by adding routing security to the network. We will use the `ndnd sec` tool to generate keys and certificates for the nodes, with an operator-controlled trust anchor.

First, we must generate the trust anchor key and certificate. On a trusted node, run:

```sh
# generate an Ed25519 root key
ndnd sec keygen /testnet ed25519 > root.key

# generate a self-signed certificate for the root key
ndnd sec sign-cert root.key < root.key > root.cert
```

The root cert is now generated. Note the `Name` of the generated certificate from the `root.cert` file. For this example, we assume the name looks like `/testnet/KEY/D%8E%F1%9C%82%19%A6a/NA/v=1737840838071`.

Next, we generate keys and certificates for the nodes. Note that in this example, we generate all keys and certificates on the same trusted node. Each node may also generate it's own key and a self-signed certificate. This self-signed certificate can then be used as a CSR to be signed by the trust anchor (pass the CSR to `stdin` of `sign-cert` instead of the key).

```sh
# generate keys for alice and bob
ndnd sec keygen /testnet/alice/32=DV ed25519 > alice.key
ndnd sec keygen /testnet/bob/32=DV ed25519 > bob.key

# generate certificates for alice and bob
ndnd sec sign-cert root.key < alice.key > alice.cert
ndnd sec sign-cert root.key < bob.key > bob.cert
```

Next, create a directory on each node to store the keys and certificates.
Copy the keys and certificates to the respective nodes.
The trust anchor certificate must be copied to all nodes in the network.
For this example, we assume the keys and certificates are stored in `/etc/ndnd/keys` on each node.

Update the configuration file on each node to use the generated keys and certificates:

```yaml
dv:
  network: /testnet
  router: /testnet/<node-name>
  keychain: dir:///etc/ndnd/keys # absolute path to the keys directory
  trust_anchors:
    - /testnet/KEY/D%8E%F1%9C%82%19%A6a/NA/v=1737840838071 # root cert name

fw:
  # same as before
```

Now, restart the NDNd daemon on each node, and establish the neighbor relationships. If the configuration is correct, the routing protocol can now communicate securely.
